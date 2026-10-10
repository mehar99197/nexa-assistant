#include "core/ai/AnthropicProvider.h"

#include "core/Logging.h"
#include "core/net/HttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kModelsTimeoutMs = 15'000;
// Inactivity limit: a model can think for a long time before the first word.
constexpr int kStreamIdleTimeoutMs = 300'000;
constexpr auto kApiVersion = "2023-06-01";

QString compact(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// Adds content blocks to the conversation, into the previous message when the role repeats: the
// API wants user and assistant to alternate, and several tool results belong in one user turn.
void appendBlocks(QJsonArray &messages, const QString &role, const QJsonArray &blocks) {
    if (blocks.isEmpty()) return;
    if (!messages.isEmpty()) {
        QJsonObject last = messages.last().toObject();
        if (last.value(u"role"_s).toString() == role) {
            QJsonArray content = last.value(u"content"_s).toArray();
            for (const QJsonValue &block : blocks) content.append(block);
            last.insert(u"content"_s, content);
            messages[messages.size() - 1] = last;
            return;
        }
    }
    messages.append(QJsonObject{{u"role"_s, role}, {u"content"_s, blocks}});
}

// A content block of a whole message, as the stream would have delivered it.
void addBlock(StreamDelta &delta, const QJsonObject &block, int index) {
    const QString type = block.value(u"type"_s).toString();
    if (type == u"text") {
        delta.text += block.value(u"text"_s).toString();
    } else if (type == u"tool_use") {
        const QJsonObject input = block.value(u"input"_s).toObject();
        delta.toolFragments.append(ToolCallFragment{.index = index,
                                                    .id = block.value(u"id"_s).toString(),
                                                    .name = block.value(u"name"_s).toString(),
                                                    .argumentsDelta = input.isEmpty() ? QString() : compact(input)});
    }
    // thinking, redacted_thinking: not shown
}

Result<QStringList> modelsFrom(const QByteArray &body) {
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) return Error{ErrorCode::Parse, u"The model list is not valid JSON"_s};
    QStringList ids;
    for (const QJsonValue &entry : document.object().value(u"data"_s).toArray()) {
        const QString id = entry.toObject().value(u"id"_s).toString();
        if (!id.isEmpty()) ids.append(id);
    }
    return ids;     // newest first, as the API lists them
}

} // namespace

AnthropicProvider::AnthropicProvider(ProviderConfig config, HttpClient *http, QObject *parent)
    : IAiProvider(parent), config_(std::move(config)), http_(http) {}

IAiProvider::Capabilities AnthropicProvider::capabilities() const {
    return Capabilities{.streaming = true, .nativeTools = !config_.textTools, .vision = true};
}

QNetworkRequest AnthropicProvider::request(const QString &path) const {
    QString base = config_.baseUrl.trimmed();
    while (base.endsWith(u'/')) base.chop(1);
    const QUrl url(base + path);
    // A gateway (OmniRoute and the like) takes the key as a bearer token. Anthropic itself reads
    // only x-api-key, and would take a bearer token for an OAuth one.
    const bool direct = url.host().endsWith(u"anthropic.com"_s);
    QNetworkRequest request = HttpClient::jsonRequest(url, direct ? QString() : config_.apiKey);
    if (!config_.apiKey.isEmpty()) request.setRawHeader("x-api-key", config_.apiKey.toUtf8());
    request.setRawHeader("anthropic-version", kApiVersion);
    return request;
}

void AnthropicProvider::listModels(std::function<void(Result<QStringList>)> done) {
    http_->get(request(u"/models?limit=1000"_s), kModelsTimeoutMs, this,
               [done = std::move(done)](Result<QByteArray> body) {
                   if (!body) {
                       done(body.error());
                       return;
                   }
                   done(modelsFrom(body.value()));
               });
}

std::shared_ptr<ChatStream> AnthropicProvider::chat(const ChatRequest &request) {
    ChatRequest effective = request;
    if (effective.model.isEmpty()) effective.model = config_.defaultModel;
    const QByteArray body = QJsonDocument(buildChatBody(effective, true)).toJson(QJsonDocument::Compact);
    QNetworkRequest httpRequest = this->request(u"/messages"_s);
    httpRequest.setRawHeader("Accept", "text/event-stream");
    QNetworkReply *reply = http_->postStreaming(httpRequest, body, kStreamIdleTimeoutMs);
    qCDebug(lcAi) << "chat" << config_.id << effective.model << effective.messages.size() << "messages";
    auto *stream = new SseChatStream(reply, &AnthropicProvider::decodeEvent, &AnthropicProvider::decodeMessage);
    return std::shared_ptr<ChatStream>(stream, [](ChatStream *done) { done->deleteLater(); });
}

QJsonObject AnthropicProvider::buildChatBody(const ChatRequest &request, bool stream) {
    QStringList system;
    QJsonArray messages;
    // The conversation must open with the user. History trimmed to fit can begin mid-exchange — with
    // a reply, or a tool result whose call was cut off — so everything before the first user turn goes.
    bool started = false;
    for (const ChatMessage &message : request.messages) {
        if (message.role == Role::System) {
            if (!message.text.trimmed().isEmpty()) system.append(message.text);
            continue;
        }
        if (!started && message.role != Role::User) continue;
        started = true;

        switch (message.role) {
        case Role::User: {
            QJsonArray blocks;
            // Images before the words about them, as the API recommends.
            for (const ImageAttachment &image : message.images) {
                blocks.append(QJsonObject{
                    {u"type"_s, u"image"_s},
                    {u"source"_s, QJsonObject{{u"type"_s, u"base64"_s},
                                              {u"media_type"_s, image.mimeType},
                                              {u"data"_s, QString::fromLatin1(image.data.toBase64())}}},
                });
            }
            // An empty text block is an error; a turn that is only images needs none.
            if (!message.text.trimmed().isEmpty()) {
                blocks.append(QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, message.text}});
            }
            appendBlocks(messages, u"user"_s, blocks);
            break;
        }
        case Role::Assistant: {
            QJsonArray blocks;
            if (!message.text.trimmed().isEmpty()) {
                blocks.append(QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, message.text}});
            }
            for (const ToolCall &call : message.toolCalls) {
                blocks.append(QJsonObject{
                    {u"type"_s, u"tool_use"_s},
                    {u"id"_s, call.id},
                    {u"name"_s, call.name},
                    // The model's arguments are an object here, not text.
                    {u"input"_s, QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object()},
                });
            }
            appendBlocks(messages, u"assistant"_s, blocks);
            break;
        }
        case Role::Tool:
            appendBlocks(messages, u"user"_s,
                         QJsonArray{QJsonObject{
                             {u"type"_s, u"tool_result"_s},
                             {u"tool_use_id"_s, message.toolCallId},
                             {u"content"_s, message.text.isEmpty() ? u"(no output)"_s : message.text},
                         }});
            break;
        case Role::System:
            break;
        }
    }

    QJsonObject body{
        {u"model"_s, request.model},
        {u"max_tokens"_s, request.maxTokens > 0 ? request.maxTokens : kDefaultMaxTokens},
        {u"messages"_s, messages},
        {u"stream"_s, stream},
    };
    if (!system.isEmpty()) body.insert(u"system"_s, system.join(u"\n\n"_s));
    if (!request.tools.isEmpty()) {
        QJsonArray tools;
        for (const ToolSpec &tool : request.tools) {
            tools.append(QJsonObject{{u"name"_s, tool.name},
                                     {u"description"_s, tool.description},
                                     {u"input_schema"_s, tool.parameters}});
        }
        body.insert(u"tools"_s, tools);
    }
    if (request.temperature >= 0) body.insert(u"temperature"_s, request.temperature);
    return body;
}

FinishReason AnthropicProvider::finishReasonFrom(const QString &stopReason) {
    if (stopReason == u"end_turn" || stopReason == u"stop_sequence") return FinishReason::Stop;
    if (stopReason == u"tool_use") return FinishReason::ToolCalls;
    if (stopReason == u"max_tokens") return FinishReason::Length;
    if (stopReason == u"refusal") return FinishReason::ContentFilter;
    return FinishReason::Other;
}

StreamDelta AnthropicProvider::decodeEvent(const SseEvent &event) {
    StreamDelta delta;
    const QJsonDocument document = QJsonDocument::fromJson(event.data);
    if (!document.isObject()) return delta;         // ping, keep-alive
    const QJsonObject root = document.object();
    // The event name and the data's own "type" agree; the data is the one a gateway keeps.
    const QString type = root.value(u"type"_s).toString(event.type);
    const int index = root.value(u"index"_s).toInt();

    if (type == u"content_block_start") {
        addBlock(delta, root.value(u"content_block"_s).toObject(), index);
    } else if (type == u"content_block_delta") {
        const QJsonObject change = root.value(u"delta"_s).toObject();
        const QString kind = change.value(u"type"_s).toString();
        if (kind == u"text_delta") {
            delta.text = change.value(u"text"_s).toString();
        } else if (kind == u"input_json_delta") {
            delta.toolFragments.append(
                ToolCallFragment{.index = index, .argumentsDelta = change.value(u"partial_json"_s).toString()});
        }
        // thinking_delta, signature_delta, citations_delta: not shown
    } else if (type == u"message_delta") {
        const QString stop = root.value(u"delta"_s).toObject().value(u"stop_reason"_s).toString();
        if (!stop.isEmpty()) delta.finishReason = finishReasonFrom(stop);
    } else if (type == u"message_stop") {
        delta.done = true;
    } else if (type == u"error") {
        const QString message = root.value(u"error"_s).toObject().value(u"message"_s).toString();
        delta.error = Error{ErrorCode::Http, message.isEmpty() ? u"The server reported an error"_s : message};
    }
    return delta;
}

StreamDelta AnthropicProvider::decodeMessage(const QByteArray &body) {
    StreamDelta delta;
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    if (root.value(u"type"_s).toString() == u"error") {
        delta.error = Error{ErrorCode::Http, HttpClient::serverMessage(body)};
        return delta;
    }
    const QJsonArray content = root.value(u"content"_s).toArray();
    for (qsizetype i = 0; i < content.size(); ++i) addBlock(delta, content.at(i).toObject(), int(i));
    const QString stop = root.value(u"stop_reason"_s).toString();
    if (!stop.isEmpty()) delta.finishReason = finishReasonFrom(stop);
    return delta;
}

} // namespace nexa

#include "core/ai/GeminiProvider.h"

#include "core/Logging.h"
#include "core/net/HttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSet>

#include <memory>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kModelsTimeoutMs = 15'000;
// Inactivity limit: a thinking model can be quiet for a long time before the first word.
constexpr int kStreamIdleTimeoutMs = 300'000;

QString compact(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// Adds parts to the conversation, into the previous turn when the role repeats: tool results and
// the words that follow them belong to one user turn.
void appendParts(QJsonArray &contents, const QString &role, const QJsonArray &parts) {
    if (parts.isEmpty()) return;
    if (!contents.isEmpty()) {
        QJsonObject last = contents.last().toObject();
        if (last.value(u"role"_s).toString() == role) {
            QJsonArray merged = last.value(u"parts"_s).toArray();
            for (const QJsonValue &part : parts) merged.append(part);
            last.insert(u"parts"_s, merged);
            contents[contents.size() - 1] = last;
            return;
        }
    }
    contents.append(QJsonObject{{u"role"_s, role}, {u"parts"_s, parts}});
}

FinishReason finishReasonFrom(const QString &value) {
    if (value == u"STOP") return FinishReason::Stop;
    if (value == u"MAX_TOKENS") return FinishReason::Length;
    static const QSet<QString> filtered{u"SAFETY"_s, u"RECITATION"_s, u"BLOCKLIST"_s, u"PROHIBITED_CONTENT"_s,
                                        u"SPII"_s, u"IMAGE_SAFETY"_s};
    if (filtered.contains(value)) return FinishReason::ContentFilter;
    return FinishReason::Other;
}

} // namespace

GeminiProvider::GeminiProvider(ProviderConfig config, HttpClient *http, QObject *parent)
    : IAiProvider(parent), config_(std::move(config)), http_(http) {}

IAiProvider::Capabilities GeminiProvider::capabilities() const {
    return Capabilities{.streaming = true, .nativeTools = !config_.textTools, .vision = true};
}

QNetworkRequest GeminiProvider::request(const QString &path) const {
    QString base = config_.baseUrl.trimmed();
    while (base.endsWith(u'/')) base.chop(1);
    const QUrl url(base + path);
    // The key goes in a header, never the URL, where logs and error messages would show it. A
    // gateway takes it as a bearer token as well.
    const bool direct = url.host().endsWith(u"googleapis.com"_s);
    QNetworkRequest request = HttpClient::jsonRequest(url, direct ? QString() : config_.apiKey);
    if (!config_.apiKey.isEmpty()) request.setRawHeader("x-goog-api-key", config_.apiKey.toUtf8());
    return request;
}

void GeminiProvider::listModels(std::function<void(Result<QStringList>)> done) {
    http_->get(request(u"/models?pageSize=1000"_s), kModelsTimeoutMs, this,
               [done = std::move(done)](Result<QByteArray> body) {
                   if (!body) {
                       done(body.error());
                       return;
                   }
                   done(parseModels(body.value()));
               });
}

std::shared_ptr<ChatStream> GeminiProvider::chat(const ChatRequest &request) {
    QString model = request.model.isEmpty() ? config_.defaultModel : request.model;
    if (model.startsWith(u"models/"_s)) model = model.mid(7);
    const QByteArray body = QJsonDocument(buildChatBody(request)).toJson(QJsonDocument::Compact);
    QNetworkRequest httpRequest = this->request(u"/models/%1:streamGenerateContent?alt=sse"_s.arg(model));
    httpRequest.setRawHeader("Accept", "text/event-stream");
    QNetworkReply *reply = http_->postStreaming(httpRequest, body, kStreamIdleTimeoutMs);
    qCDebug(lcAi) << "chat" << config_.id << model << request.messages.size() << "messages";

    // Calls arrive whole, one event after another: each takes the next slot.
    auto calls = std::make_shared<int>(0);
    const auto decodeEvent = [calls](const SseEvent &event) {
        const StreamDelta delta = decodeResponse(QJsonDocument::fromJson(event.data).object(), *calls);
        *calls += int(delta.toolFragments.size());
        return delta;
    };
    auto *stream = new SseChatStream(reply, decodeEvent, &GeminiProvider::decodeBody);
    return std::shared_ptr<ChatStream>(stream, [](ChatStream *done) { done->deleteLater(); });
}

QJsonObject GeminiProvider::schema(const QJsonObject &jsonSchema) {
    static const QSet<QString> kept{u"type"_s,      u"format"_s,   u"description"_s, u"nullable"_s,
                                    u"enum"_s,      u"required"_s, u"minimum"_s,     u"maximum"_s,
                                    u"minItems"_s,  u"maxItems"_s, u"minLength"_s,   u"maxLength"_s};
    QJsonObject cleaned;
    for (auto it = jsonSchema.constBegin(); it != jsonSchema.constEnd(); ++it) {
        if (kept.contains(it.key())) cleaned.insert(it.key(), it.value());
    }
    const QJsonObject properties = jsonSchema.value(u"properties"_s).toObject();
    if (!properties.isEmpty()) {
        QJsonObject children;
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
            children.insert(it.key(), schema(it.value().toObject()));
        }
        cleaned.insert(u"properties"_s, children);
    }
    if (jsonSchema.value(u"items"_s).isObject()) cleaned.insert(u"items"_s, schema(jsonSchema.value(u"items"_s).toObject()));
    // An object with nothing in it is refused: the caller leaves "parameters" out instead.
    if (cleaned.value(u"type"_s).toString() == u"object" && !cleaned.contains(u"properties"_s)) return {};
    return cleaned;
}

QJsonObject GeminiProvider::buildChatBody(const ChatRequest &request) {
    QStringList system;
    QJsonArray contents;
    // The conversation must open with the user (see AnthropicProvider::buildChatBody).
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
            QJsonArray parts;
            if (!message.text.trimmed().isEmpty()) parts.append(QJsonObject{{u"text"_s, message.text}});
            for (const ImageAttachment &image : message.images) {
                parts.append(QJsonObject{{u"inlineData"_s, QJsonObject{{u"mimeType"_s, image.mimeType},
                                                                      {u"data"_s, QString::fromLatin1(image.data.toBase64())}}}});
            }
            appendParts(contents, u"user"_s, parts);
            break;
        }
        case Role::Assistant: {
            QJsonArray parts;
            if (!message.text.trimmed().isEmpty()) parts.append(QJsonObject{{u"text"_s, message.text}});
            for (const ToolCall &call : message.toolCalls) {
                QJsonObject part{{u"functionCall"_s,
                                  QJsonObject{{u"name"_s, call.name},
                                              {u"args"_s, QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object()}}}};
                // A thinking model refuses the next turn unless its signature comes back with the call.
                if (!call.signature.isEmpty()) part.insert(u"thoughtSignature"_s, call.signature);
                parts.append(part);
            }
            appendParts(contents, u"model"_s, parts);
            break;
        }
        case Role::Tool:
            // Matched to its call by name; the response has to be an object.
            appendParts(contents, u"user"_s,
                        QJsonArray{QJsonObject{{u"functionResponse"_s,
                                                QJsonObject{{u"name"_s, message.toolName},
                                                            {u"response"_s, QJsonObject{{u"content"_s, message.text}}}}}}});
            break;
        case Role::System:
            break;
        }
    }

    QJsonObject body{{u"contents"_s, contents}};
    if (!system.isEmpty()) {
        body.insert(u"systemInstruction"_s,
                    QJsonObject{{u"parts"_s, QJsonArray{QJsonObject{{u"text"_s, system.join(u"\n\n"_s)}}}}});
    }
    if (!request.tools.isEmpty()) {
        QJsonArray declarations;
        for (const ToolSpec &tool : request.tools) {
            QJsonObject declaration{{u"name"_s, tool.name}, {u"description"_s, tool.description}};
            if (const QJsonObject parameters = schema(tool.parameters); !parameters.isEmpty()) {
                declaration.insert(u"parameters"_s, parameters);
            }
            declarations.append(declaration);
        }
        body.insert(u"tools"_s, QJsonArray{QJsonObject{{u"functionDeclarations"_s, declarations}}});
    }
    QJsonObject generation;
    if (request.temperature >= 0) generation.insert(u"temperature"_s, request.temperature);
    if (request.maxTokens > 0) generation.insert(u"maxOutputTokens"_s, request.maxTokens);
    if (!generation.isEmpty()) body.insert(u"generationConfig"_s, generation);
    return body;
}

StreamDelta GeminiProvider::decodeResponse(const QJsonObject &response, int firstCallIndex) {
    StreamDelta delta;
    if (response.contains(u"error"_s)) {
        const QString message = response.value(u"error"_s).toObject().value(u"message"_s).toString();
        delta.error = Error{ErrorCode::Http, message.isEmpty() ? u"The server reported an error"_s : message};
        return delta;
    }
    const QJsonArray candidates = response.value(u"candidates"_s).toArray();
    if (candidates.isEmpty()) {
        // A request refused outright comes back with no candidates, only the reason.
        const QString blocked = response.value(u"promptFeedback"_s).toObject().value(u"blockReason"_s).toString();
        if (!blocked.isEmpty()) {
            delta.error = Error{ErrorCode::Http, u"Gemini refused the request (%1)."_s.arg(blocked)};
        }
        return delta;
    }
    const QJsonObject candidate = candidates.first().toObject();
    int index = firstCallIndex;
    const QJsonArray parts = candidate.value(u"content"_s).toObject().value(u"parts"_s).toArray();
    for (const QJsonValue &value : parts) {
        const QJsonObject part = value.toObject();
        if (part.value(u"thought"_s).toBool()) continue;       // a thought summary is not the answer
        if (part.contains(u"functionCall"_s)) {
            const QJsonObject call = part.value(u"functionCall"_s).toObject();
            delta.toolFragments.append(ToolCallFragment{.index = index++,
                                                        .id = call.value(u"id"_s).toString(),
                                                        .name = call.value(u"name"_s).toString(),
                                                        .argumentsDelta = compact(call.value(u"args"_s).toObject()),
                                                        .signature = part.value(u"thoughtSignature"_s).toString()});
        } else if (part.contains(u"text"_s)) {
            delta.text += part.value(u"text"_s).toString();
        }
    }
    const QString finish = candidate.value(u"finishReason"_s).toString();
    if (!finish.isEmpty()) delta.finishReason = finishReasonFrom(finish);
    return delta;
}

StreamDelta GeminiProvider::decodeBody(const QByteArray &body) {
    const QJsonDocument document = QJsonDocument::fromJson(body);
    QJsonArray responses = document.isArray() ? document.array() : QJsonArray{document.object()};
    StreamDelta whole;
    int calls = 0;
    for (const QJsonValue &value : responses) {
        const StreamDelta part = decodeResponse(value.toObject(), calls);
        if (part.error) return part;
        whole.text += part.text;
        whole.toolFragments += part.toolFragments;
        calls += int(part.toolFragments.size());
        if (part.finishReason) whole.finishReason = part.finishReason;
    }
    return whole;
}

Result<QStringList> GeminiProvider::parseModels(const QByteArray &body) {
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) return Error{ErrorCode::Parse, u"The model list is not valid JSON"_s};
    QStringList ids;
    for (const QJsonValue &entry : document.object().value(u"models"_s).toArray()) {
        const QJsonObject model = entry.toObject();
        // Only models that can chat: embedding and image models are listed too.
        const QJsonArray methods = model.value(u"supportedGenerationMethods"_s).toArray();
        if (!methods.contains(u"generateContent"_s)) continue;
        QString name = model.value(u"name"_s).toString();
        if (name.startsWith(u"models/"_s)) name = name.mid(7);
        if (!name.isEmpty()) ids.append(name);
    }
    ids.sort(Qt::CaseInsensitive);
    return ids;
}

} // namespace nexa

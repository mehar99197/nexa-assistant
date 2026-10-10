#pragma once

#include "core/Result.h"
#include "core/ai/IAiProvider.h"
#include "core/ai/SseChatStream.h"
#include "core/config/Config.h"

#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrl>

namespace nexa {

class HttpClient;

// Provider for Google's Gemini API — POST {baseUrl}/models/{model}:streamGenerateContent?alt=sse.
// The differences from the OpenAI format all live here: the system prompt is systemInstruction,
// roles are "user" and "model", a reply is a list of parts (text, functionCall), a tool's result is
// a functionResponse part matched to its call by name, calls arrive whole rather than in pieces, a
// thinking model's thoughtSignature has to come back with its call, and tool schemas are an OpenAPI
// subset that refuses JSON Schema keywords it does not know.
class GeminiProvider : public IAiProvider {
    Q_OBJECT
public:
    GeminiProvider(ProviderConfig config, HttpClient *http, QObject *parent = nullptr);

    QString id() const override { return config_.id; }
    QString displayName() const override { return config_.name; }
    Capabilities capabilities() const override;
    void listModels(std::function<void(Result<QStringList>)> done) override;
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;

    // Pure wire-format helpers (unit-tested).
    static QJsonObject buildChatBody(const ChatRequest &request);
    // One GenerateContentResponse. Calls are numbered from `firstCallIndex`: they arrive whole, one
    // event after another, so each needs a slot of its own.
    static StreamDelta decodeResponse(const QJsonObject &response, int firstCallIndex = 0);
    // A whole answer: one response object, or the array an unstreamed request returns.
    static StreamDelta decodeBody(const QByteArray &body);
    static Result<QStringList> parseModels(const QByteArray &body);
    // A tool's JSON Schema reduced to what Gemini accepts. Empty when the tool takes no arguments,
    // since Gemini refuses an object schema without properties.
    static QJsonObject schema(const QJsonObject &jsonSchema);

private:
    QNetworkRequest request(const QString &path) const;

    ProviderConfig config_;
    HttpClient *http_;
};

} // namespace nexa

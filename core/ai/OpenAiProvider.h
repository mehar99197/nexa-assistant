#pragma once

#include "core/Result.h"
#include "core/ai/ChatStream.h"
#include "core/ai/IAiProvider.h"
#include "core/ai/ToolCallAccumulator.h"
#include "core/config/Config.h"

#include <QJsonObject>
#include <QUrl>

#include <optional>

namespace nexa {

class HttpClient;

// Provider for every OpenAI-compatible API: OmniRoute (the default), OpenAI, Ollama, LM Studio,
// Groq, OpenRouter, DeepSeek, Mistral, xAI, Together, … — POST {baseUrl}/chat/completions, stream=true.
class OpenAiProvider : public IAiProvider {
    Q_OBJECT
public:
    // What one stream chunk (or a whole non-streamed completion) contributes to the reply.
    struct Delta {
        QString text;
        QList<ToolCallFragment> toolFragments;
        std::optional<FinishReason> finishReason;
        bool done{false};                           // the "[DONE]" sentinel
        std::optional<Error> error;
    };

    OpenAiProvider(ProviderConfig config, HttpClient *http, QObject *parent = nullptr);

    QString id() const override { return config_.id; }
    QString displayName() const override { return config_.name; }
    Capabilities capabilities() const override;
    void listModels(std::function<void(Result<QStringList>)> done) override;
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;

    // Pure wire-format helpers (unit-tested).
    static QJsonObject buildChatBody(const ChatRequest &request, bool stream);
    static Delta decodeChunk(const QByteArray &data);
    static Result<QStringList> parseModels(const QByteArray &body);

private:
    QUrl endpoint(const QString &path) const;

    ProviderConfig config_;
    HttpClient *http_;
};

} // namespace nexa

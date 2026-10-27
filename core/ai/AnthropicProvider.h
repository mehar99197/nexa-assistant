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

// Provider for Anthropic's Messages API — POST {baseUrl}/messages with stream=true — and gateways
// that speak it. The differences from the OpenAI format all live here: the system prompt is a
// top-level field, a reply is a list of content blocks (text, tool_use), a tool's result is a
// tool_result block in the next user turn, roles must alternate, max_tokens is required, and the
// stream arrives as typed events (content_block_start/delta/stop, message_delta, message_stop).
class AnthropicProvider : public IAiProvider {
    Q_OBJECT
public:
    static constexpr int kDefaultMaxTokens = 8192;

    AnthropicProvider(ProviderConfig config, HttpClient *http, QObject *parent = nullptr);

    QString id() const override { return config_.id; }
    QString displayName() const override { return config_.name; }
    Capabilities capabilities() const override;
    void listModels(std::function<void(Result<QStringList>)> done) override;
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;

    // Pure wire-format helpers (unit-tested).
    static QJsonObject buildChatBody(const ChatRequest &request, bool stream);
    static StreamDelta decodeEvent(const SseEvent &event);
    // A whole, unstreamed message — what a gateway that ignores stream=true sends.
    static StreamDelta decodeMessage(const QByteArray &body);
    static FinishReason finishReasonFrom(const QString &stopReason);

private:
    QNetworkRequest request(const QString &path) const;
    QNetworkRequest requestFor(const QUrl &url) const;

    ProviderConfig config_;
    HttpClient *http_;
};

} // namespace nexa

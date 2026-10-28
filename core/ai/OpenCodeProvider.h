#pragma once

#include "core/ai/IAiProvider.h"
#include "core/ai/OpenCodeWire.h"
#include "core/ai/TurnLedger.h"
#include "core/config/Config.h"

#include <QHash>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <optional>

namespace nexa {

class HttpClient;
class OpenCodeServer;
class OpenCodeStream;

// Provider for opencode and every model signed in there — GitHub Copilot, Google's Antigravity
// through its plugin, OpenRouter, … — on opencode's own credentials (Strategy). It talks to
// OpenCodeServer's HTTP API: one opencode session per conversation, TurnLedger deciding what the
// session still needs, Nexa's system prompt in `system` and opencode's own tools off in every
// prompt; Nexa's tools travel as text (nativeTools = false). An empty model is opencode's own default.
class OpenCodeProvider : public IAiProvider {
    Q_OBJECT
public:
    OpenCodeProvider(ProviderConfig config, HttpClient *http, std::shared_ptr<OpenCodeServer> server,
                     QObject *parent = nullptr);
    ~OpenCodeProvider() override;

    QString id() const override { return config_.id; }
    QString displayName() const override { return config_.name; }
    Capabilities capabilities() const override;
    void listModels(std::function<void(Result<QStringList>)> done) override;
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;
    void warmUp() override;

    // How long a turn may go without an event of its session before it fails (Timeout) — a missed
    // idle while the event stream was being reopened, a provider that hangs; 5 minutes.
    void setTurnTimeoutMs(int ms) { turnTimeoutMs_ = ms; }

private:
    void sendTurn();
    void prompt(const QString &session, const QJsonObject &body);
    void onEvent(const OpenCodeEvent &event);
    void finishTurn(std::optional<Error> error);
    void abortSession();

    ProviderConfig config_;
    HttpClient *http_;
    std::shared_ptr<OpenCodeServer> server_;
    TurnLedger ledger_;
    QString sessionId_;
    QPointer<OpenCodeStream> stream_;
    ChatRequest pending_;           // the request of the turn on its way
    bool sawBusy_{false};           // the session has started on this turn: its next idle ends it
    QHash<QString, QString> partTypes_;     // this turn's parts: only "text" ones reach the reply
    QTimer retryLimit_;
    QTimer turnTimer_;
    int turnTimeoutMs_{300'000};
};

} // namespace nexa

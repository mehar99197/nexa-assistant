#pragma once

#include "core/ai/IAiProvider.h"
#include "core/ai/OpenCodeWire.h"
#include "core/ai/TurnLedger.h"
#include "core/config/Config.h"

#include <QHash>
#include <QJsonObject>
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
    std::optional<bool> takesImages(const QString &model) const override;
    QStringList imageModels() const override;

    // How long a turn may go without an event of its session before it fails (Timeout) — a missed
    // idle while the event stream was being reopened, a provider that hangs; 5 minutes.
    void setTurnTimeoutMs(int ms) { turnTimeoutMs_ = ms; }

private:
    void sendTurn();
    void prompt(const QString &session, const QJsonObject &body);
    void onEvent(const OpenCodeEvent &event);
    void finishTurn(std::optional<Error> error);
    void abortSession();
    void showText(const QString &text);
    void noteToolPart(const OpenCodeEvent &event);
    void handOverNativeCalls();
    void sendDeferredPrompt();
    bool failIfRetired();               // the held text is a "no longer available" notice: fail the turn
    QString modelName() const;

    ProviderConfig config_;
    HttpClient *http_;
    std::shared_ptr<OpenCodeServer> server_;
    TurnLedger ledger_;
    QString sessionId_;
    QPointer<OpenCodeStream> stream_;
    ChatRequest pending_;           // the request of the turn on its way
    bool sawBusy_{false};           // the session has started on this turn: its next idle ends it
    QHash<QString, QString> partTypes_;     // this turn's parts: only "text" ones reach the reply
    // The start of a reply, held until it cannot be a retired model's notice (which the Antigravity
    // plugin repeats as the reply for minutes).
    QString held_;
    bool holding_{true};
    // Tools the model called natively that opencode does not have — Nexa's, which DeepSeek reaches for
    // when opencode's read tool is offered: by part, name and arguments, handed over at the step's end.
    QList<std::pair<QString, std::pair<QString, QString>>> nativeCalls_;
    // After Nexa stopped a step to run the tools it called, opencode still reports that step's end (idle,
    // an abort error) — late, and it would end the next step. The next prompt waits for it (5 s at most).
    bool settling_{false};
    std::optional<std::pair<QString, QJsonObject>> deferredPrompt_;
    QTimer settleTimer_;
    QHash<QString, bool> images_;           // "provider/model" → takes images, from the last model list
    QTimer retryLimit_;
    QTimer turnTimer_;
    int turnTimeoutMs_{300'000};
};

} // namespace nexa

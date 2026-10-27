#pragma once

#include "core/ai/AgyEvents.h"
#include "core/ai/IAiProvider.h"
#include "core/ai/TurnLedger.h"
#include "core/config/Config.h"

#include <QPointer>
#include <QStringList>

#include <memory>

namespace nexa {

class AgySession;
class AgyStream;

// Provider for agy, Google's Antigravity CLI, on the user's own sign-in and quota (Strategy). One warm
// agy process holds the conversation (AgySession) and TurnLedger decides what it still needs to be
// told. agy cannot take Nexa's tools natively, so they travel as text (nativeTools = false) and agy's
// own tools stay refused, as print mode does without --dangerously-skip-permissions. `baseUrl` in the
// config names the program; empty finds it on the PATH or in ~/.local/bin.
class AgyProvider : public IAiProvider {
    Q_OBJECT
public:
    // `workingDir` empty: ~/.cache/nexa/agy, an empty folder of agy's own.
    explicit AgyProvider(ProviderConfig config, QString workingDir = {}, QObject *parent = nullptr);
    ~AgyProvider() override;

    QString id() const override { return config_.id; }
    QString displayName() const override { return config_.name; }
    Capabilities capabilities() const override;
    void listModels(std::function<void(Result<QStringList>)> done) override;
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;
    void warmUp() override;

private:
    QString program() const;
    void replaceSession(const QString &program, const QString &model);
    void onTurnFinished(const AgyEvent &result);

    ProviderConfig config_;
    QString workingDir_;
    std::unique_ptr<AgySession> session_;
    TurnLedger ledger_;
    QPointer<AgyStream> stream_;
    ChatRequest pending_;           // the request of the turn on its way
    bool reminded_{false};
    QStringList models_;
};

} // namespace nexa

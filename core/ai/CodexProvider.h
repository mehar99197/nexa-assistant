#pragma once

#include "core/ai/CodexSession.h"
#include "core/ai/IAiProvider.h"
#include "core/ai/TurnLedger.h"
#include "core/config/Config.h"

#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace nexa {

class CodexStream;

// Provider for Codex, OpenAI's coding agent, on the user's own ChatGPT sign-in and plan (Strategy). One warm
// `codex app-server` holds the conversation (CodexSession) and TurnLedger decides what its thread still needs
// to be told. Codex's own tools are switched off at the command line and Nexa's tools travel as text
// (nativeTools = false), as with agy; a model that reaches for its own is stopped and reminded once. `baseUrl`
// in the config names the program; empty finds it on the PATH, in ~/.local/bin or in ~/.npm-global/bin (where
// npm puts it). An empty model is the first one Codex lists as visible.
class CodexProvider : public IAiProvider {
    Q_OBJECT
public:
    // `workingDir` empty: ~/.cache/nexa/codex, whose `work` folder is the empty one Codex stands in.
    explicit CodexProvider(ProviderConfig config, QString workingDir = {}, QObject *parent = nullptr);
    ~CodexProvider() override;

    QString id() const override { return config_.id; }
    QString displayName() const override { return config_.name; }
    Capabilities capabilities() const override;
    // Needs the sign-in: Codex's own model list answers without one, with models the account cannot use.
    void listModels(std::function<void(Result<QStringList>)> done) override;
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;
    void warmUp() override;
    std::optional<bool> takesImages(const QString &) const override { return false; }   // its input is text only

    // How long a turn may go without a sign of life (thinking counts) before it fails (Timeout), so the chain
    // can ask another provider; 5 minutes, like the HTTP providers' streams.
    void setTurnTimeoutMs(int ms) { turnTimeoutMs_ = ms; }

    // The `codex` program: `configured` when it names one that can be run, else where it is installed.
    static QString locate(const QString &configured);

private:
    QString program() const;
    void replaceSession(const QString &program);
    void retireSession();
    void onTurnFinished(const CodexTurnResult &result);

    ProviderConfig config_;
    QString workingDir_;
    std::unique_ptr<CodexSession> session_;
    TurnLedger ledger_;
    QPointer<CodexStream> stream_;
    QObject *activeStream_{nullptr};    // the stream of the latest request: compared, never used
    ChatRequest pending_;           // the request of the turn on its way
    bool reminded_{false};
    QTimer turnTimer_;
    int turnTimeoutMs_{300'000};
    QStringList models_;
};

} // namespace nexa

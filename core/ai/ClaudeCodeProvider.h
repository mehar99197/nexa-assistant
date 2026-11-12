#pragma once

#include "core/ai/ClaudeCodeEvents.h"
#include "core/ai/IAiProvider.h"
#include "core/ai/TurnLedger.h"
#include "core/config/Config.h"

#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace nexa {

class ClaudeCodeSession;
class ClaudeCodeStream;

// Provider for Claude Code, Anthropic's coding agent, on the owner's own Claude subscription (Strategy). One warm
// `claude -p` process holds the conversation (ClaudeCodeSession) and TurnLedger decides what it still needs to be told;
// a changed system prompt or model is a new process, since both are start flags of the program. Claude Code's own
// tools are switched off at the command line and Nexa's tools travel as text (nativeTools = false), as with agy and
// Codex; nothing of its own can prompt. Its models see pictures (vision = true). `baseUrl` in the config names the
// program; empty finds it on the PATH, in ~/.local/bin or in ~/.npm-global/bin. An empty model is Claude Code's own
// default; the toolbar offers its aliases (fable, opus, sonnet, haiku — there is no list command).
//
// Legal basis (https://code.claude.com/docs/en/legal-and-compliance, read 2026-11-12): a third-party app may not offer
// Claude.ai login, route requests through a user's subscription on their behalf, or collect, store or intermediate
// Claude.ai credentials; an end user may sign in to the unmodified Claude Code binary with their own subscription,
// also where a platform hosts it, through Anthropic's own flow. So Nexa runs the unmodified binary as published, never
// reads, stores or forwards its credentials (no CLAUDE_CONFIG_DIR of Nexa's, no `--bare`), and the sign-in is Claude
// Code's own `claude auth login` (ClaudeCodeSignIn). The Anthropic API-key preset stays for API use.
class ClaudeCodeProvider : public IAiProvider {
    Q_OBJECT
public:
    // `workingDir` empty: ~/.cache/nexa/claude, whose `work` folder is the empty one Claude Code stands in.
    explicit ClaudeCodeProvider(ProviderConfig config, QString workingDir = {}, QObject *parent = nullptr);
    ~ClaudeCodeProvider() override;

    QString id() const override { return config_.id; }
    QString displayName() const override { return config_.name; }
    Capabilities capabilities() const override;
    // The aliases Claude Code takes; NotFound when the program is not installed.
    void listModels(std::function<void(Result<QStringList>)> done) override;
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;
    void warmUp() override;
    std::optional<bool> takesImages(const QString &) const override { return true; }     // every Claude model sees
    QStringList imageModels() const override { return ClaudeCodeEvents::knownModels(); }

    // How long a turn may go without a sign of life (thinking counts) before it fails (Timeout), so the chain can ask
    // another provider; 5 minutes, like the HTTP providers' streams.
    void setTurnTimeoutMs(int ms) { turnTimeoutMs_ = ms; }

    // The `claude` program: `configured` when it names one that can be run, else where it is installed.
    static QString locate(const QString &configured);

private:
    QString program() const;
    void replaceSession(const QString &program, const QString &model, const QString &systemPrompt);
    void onTurnFinished(const ClaudeCodeEvent &result);

    ProviderConfig config_;
    QString workingDir_;
    std::unique_ptr<ClaudeCodeSession> session_;
    TurnLedger ledger_;
    QPointer<ClaudeCodeStream> stream_;
    ChatRequest pending_;           // the request of the turn on its way
    QTimer turnTimer_;
    int turnTimeoutMs_{300'000};
};

} // namespace nexa

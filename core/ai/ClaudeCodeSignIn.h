#pragma once

#include "core/ai/SignIn.h"

#include <QByteArray>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace nexa {

// Sign-in for Claude Code through the program's own commands (Strategy; Facade over short-lived processes).
//  status:   `claude auth status --json` — loggedIn, authMethod and subscriptionType are read: the plan ("Claude · Max"),
//            never the email or the organisation the JSON also carries (ClaudeCodeEvents::parseStatus)
//  sign in:  `claude auth login --claudeai`, Claude Code's own OAuth: it opens the browser itself and prints the link,
//            which Nexa shows as it does for Codex (never logs), then waits on its standard input for the code the
//            page shows when its callback cannot reach it — submitCode() types it (pendingNeedsCode while it waits);
//            the callback may finish the login instead, so the process ending well is the end either way, and the
//            status check then says what Claude Code now says, not what a clean exit seems to mean
//  sign out: `claude auth logout`, then the status check the same way
// The sign-in is the one the owner's terminal has — Claude Code's own folder, which Nexa never points elsewhere and
// never reads (Anthropic's terms: the user signs in to the unmodified Claude Code themselves; no third-party app may
// collect, store or intermediate the credentials) — so signing out here signs out there too. One operation at a time;
// cancel() ends the process group and fails with Cancelled. Nothing the program prints is logged, and nothing of it is
// kept past the operation; Error::message carries Nexa's words and an exit code, never the program's.
class ClaudeCodeSignIn : public ISignIn {
    Q_OBJECT
public:
    static constexpr int kStepTimeoutMs = 20'000;
    static constexpr int kSignInTimeoutMs = 5 * 60 * 1000;

    // `program` is Claude Code's path (empty: it is not installed); `workingDir` defaults to the folder
    // ClaudeCodeProvider uses, `<GenericCache>/nexa/claude`, whose `work` folder the commands run in.
    explicit ClaudeCodeSignIn(QString program, QString workingDir = {}, QObject *parent = nullptr);
    ~ClaudeCodeSignIn() override;

    bool busy() const override { return op_ != Op::None; }
    bool pendingNeedsCode() const override { return op_ == Op::SignIn && step_ == Step::Login; }

    // How long one command may take, and how long the browser sign-in may (tests make them short).
    void setTimeouts(int stepMs, int signInMs);

public slots:
    void checkStatus() override;
    void listMethods() override;
    void signIn(const QString &methodId = {}) override;
    void submitCode(const QString &code) override;
    void signOut() override;
    void cancel() override;

private:
    enum class Op { None, Status, SignIn, SignOut };
    enum class Step { None, Status, Login, Logout };

    static QString name(Op op);
    bool begin(Op op);                                      // false (and a failure out or on its way) when it cannot
    void run(const QStringList &arguments, Step step, int timeoutMs);
    void readLogin();
    void onProcessEnded(int exitCode);
    void onStatusAnswered(int exitCode, const QByteArray &output);
    void onTimeout();
    void finish(const std::function<void()> &emitResult);
    void fail(const Error &error);
    void defer(const Error &error);
    void stopProcess();

    QString program_;
    QString workingDir_;
    int stepTimeoutMs_{kStepTimeoutMs};
    int signInTimeoutMs_{kSignInTimeoutMs};
    Op op_{Op::None};
    Step step_{Step::None};
    QPointer<QProcess> process_;
    qint64 groupId_{0};             // the process group of process_ (its pid: it leads its own session)
    QTimer timer_;
    QString loginOutput_;           // what the login printed so far, scanned for the link and the prompt; never logged
    bool linkShown_{false};
    bool codeSent_{false};
    quint64 generation_{0};
};

} // namespace nexa

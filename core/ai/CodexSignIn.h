#pragma once

#include "core/ai/CodexProtocol.h"
#include "core/ai/CodexSession.h"
#include "core/ai/SignIn.h"

#include <QString>
#include <QTimer>

#include <memory>

namespace nexa {

// Sign-in for Codex, through the program's own `codex app-server` (Strategy; Facade over a short-lived server).
// *Status* is `account/read`; *sign in* is `account/login/start` — Codex runs the loopback callback itself, so
// Nexa shows its link (never logs it) and waits for `account/login/completed`, up to 5 minutes, cancellable
// (`account/login/cancel`, then the process group); *sign out* is `account/logout`. The token stays in the
// owner's CODEX_HOME, shared with their terminal Codex; the email in `account/read` is never read. No thread
// is started, so none of the user's MCP servers is either. One operation at a time.
class CodexSignIn : public ISignIn {
    Q_OBJECT
public:
    // `workingDir` empty: ~/.cache/nexa/codex (the same as CodexProvider's, for the server's empty folder).
    explicit CodexSignIn(QString program, QString workingDir = {}, QObject *parent = nullptr);
    ~CodexSignIn() override;

    bool busy() const override { return busy_; }

    void setLoginTimeoutMs(int ms) { loginTimeoutMs_ = ms; }
    void setStepTimeoutMs(int ms) { stepTimeoutMs_ = ms; }

public slots:
    void checkStatus() override;
    void listMethods() override;
    void signIn(const QString &methodId = {}) override;
    void submitCode(const QString &code) override;
    void signOut() override;
    void cancel() override;

private:
    enum class Op { None, Status, SignIn, SignOut };

    bool begin(Op op);                              // false (and a failure on its way) when it cannot
    void failSoon(const Error &error);
    void fail(const Error &error);
    void end();                                     // the operation is over: drop the server
    void onNotification(const CodexMessage &message);
    void onClosed(const Error &error);
    void readAccountThen(const std::function<void(const SignInStatus &)> &next);
    void abandon();                                 // let the server go, after it has heard a login cancel

    QString program_;
    QString workingDir_;
    std::unique_ptr<CodexConnection> connection_;
    QTimer timer_;
    Op op_{Op::None};
    QString loginId_;
    quint64 serial_{0};
    int loginTimeoutMs_{300'000};
    int stepTimeoutMs_{20'000};
    bool busy_{false};
    bool waitingForLogin_{false};
};

} // namespace nexa

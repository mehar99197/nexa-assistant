#pragma once

#include "core/ai/AgyAcp.h"
#include "core/ai/SignIn.h"

#include <QByteArray>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

namespace nexa {

// Strategy (ISignIn) and Facade over `agy acp`, Google's Antigravity CLI as an Agent Client Protocol
// server. agy does the OAuth — it opens the browser itself and keeps the token in the system keyring —
// so no token passes through Nexa; Nexa starts the program, asks, and reports. Each operation runs its own
// short-lived `agy acp` in its own process group, with the private home and workspace AgySession::prepare
// makes, and ends it (the whole group: agy starts a language server) when the operation is over, on
// cancel() and on a timeout.
//
//  status:   initialize, session/new — "Authentication required" is signed out, a result carries the plan
//            (never an account) — then session/close
//  methods:  initialize; the browser sign-ins it offers
//  sign in:  initialize, authenticate (the reply comes after the browser's callback: five minutes, and
//            cancel() ends the wait), then the status check, so "signed in" is what agy now says, not what
//            an unmeasured reply seems to mean
//  sign out: initialize, logout (only if agy says it can), then the status check
//
// Anything agy says that is not understood is failed(Parse) or failed(Unknown), never "signed out". The
// sign-in is in the keyring the user's own terminal agy shares, so signing out here signs out there too.
// Nothing of an authenticate exchange is logged (method names and error codes only), and agy's words in an
// error are shown only after AgyAcp::safeText.
class AgySignIn : public ISignIn {
    Q_OBJECT
public:
    static constexpr int kRequestTimeoutMs = 20'000;
    static constexpr int kSignInTimeoutMs = 5 * 60 * 1000;

    // `program` is agy's path (empty: it is not installed); `workingDir` defaults to the folder AgyProvider
    // uses, `<GenericCache>/nexa/agy`, so the sign-in runs in the home whose settings deny agy's own tools.
    explicit AgySignIn(QString program, QString workingDir = {}, QObject *parent = nullptr);
    ~AgySignIn() override;

    bool busy() const override { return operation_ != Operation::None; }

    // How long one request may take, and how long the browser sign-in may (tests make them short).
    void setTimeouts(int requestMs, int signInMs);

public slots:
    void checkStatus() override;
    void listMethods() override;
    void signIn(const QString &methodId = {}) override;
    void submitCode(const QString &code) override;      // agy never asks for a pasted code: failed(NotSupported)
    void signOut() override;
    void cancel() override;

private:
    enum class Operation { None, Status, Methods, SignIn, SignOut };
    enum class Step { None, Initialize, NewSession, CloseSession, Authenticate, Logout };

    static QString name(Operation operation);
    bool refuseWhileBusy();     // a second operation: failed(InvalidArgument), at once
    bool begin(Operation operation);
    void defer(const Error &error);
    void send(int id, const QByteArray &line, const QString &method, Step step, int timeoutMs);
    void openSession();
    void readOutput();
    void handle(const AcpMessage &message);
    void onInitialized(const AcpMessage &message);
    void onSession(const AcpMessage &message);
    void onAuthenticated(const AcpMessage &message);
    void onLoggedOut(const AcpMessage &message);
    void onProcessEnded(int exitCode);
    void onTimeout();
    void closeSessionThen(const QString &sessionId, std::function<void()> then);
    void finish(const std::function<void()> &emitResult);
    void fail(const Error &error);
    void stopProcess();

    QString program_;
    QString workingDir_;
    int requestTimeoutMs_{kRequestTimeoutMs};
    int signInTimeoutMs_{kSignInTimeoutMs};

    Operation operation_{Operation::None};
    Step step_{Step::None};
    QString methodId_;
    std::optional<AcpInitialize> initialize_;
    int nextId_{1};
    int pendingId_{-1};
    QString pendingMethod_;
    quint64 generation_{0};
    bool linkShown_{false};
    std::function<void()> afterClose_;
    QPointer<QProcess> process_;
    qint64 groupId_{0};         // the process group of process_ (its pid: it leads its own session)
    QTimer timer_;
    QByteArray buffer_;
};

} // namespace nexa

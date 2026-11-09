#pragma once

#include "core/ai/OpenCodeWire.h"
#include "core/ai/SignIn.h"

#include <QByteArray>
#include <QList>
#include <QNetworkReply>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace nexa {

class OpenCodeServer;

// Strategy (ISignIn) and Facade over the opencode server Nexa already runs (`OpenCodeServer`, used as it is):
// the browser sign-ins opencode offers by itself — ChatGPT Pro/Plus, GitHub Copilot, Poe, xAI … — through its
// own server API, so opencode does the OAuth and writes the login (~/.local/share/opencode/auth.json, which the
// user's terminal opencode shares) and no token, key or code ever rests in Nexa.
//
// What is offered is an allow-list (`providers()`): only providers a clean opencode lists by itself, only their
// OAuth entries, and — for this milestone — only those that ask no questions first (an enterprise URL, an
// account name …). A provider a plugin adds (the owner's `opencode-antigravity-auth` adds a Google login Google
// did not make) is ignored, whatever the server lists.
//
// Flow of signIn("<provider>#<index>"): wait for the server (about 13 s on a fresh start) → GET /provider/auth
// to check the entry is one that is offered → POST …/oauth/authorize {method} → linkReady(url, words, false):
// the caller opens the link (opencode opens nothing) →
//   "auto":  POST …/oauth/callback {method} is held until the browser flow finished (6 minutes at most);
//   "code":  the caller shows a code field (`pendingNeedsCode()`) and passes the pasted code to submitCode(),
//            which posts {method, code};
// → on `true`: POST /instance/dispose (opencode caches its providers per instance, as its own TUI knows), the
// user's MCP servers that the new instance connects again left out once more (as OpenCodeServer does at its
// start; a server the user runs is left alone), and signedIn(status). A failed code or flow ends the operation; starting again authorizes anew (opencode keeps
// one pending flow per provider; a new authorize replaces it).
//
// One operation at a time (`busy()`). A call made meanwhile is refused with failed() — before it returns, and
// without disturbing the running operation — so the caller checks busy() first. cancel() ends the operation at
// once with failed(Cancelled) and aborts the held request; it cannot make opencode drop its side of a browser
// flow (a listener opencode started stays until the server ends), which a later authorize replaces.
//
// Nothing here is logged but provider ids and method indexes: not a link (its query holds state and PKCE), a
// code, opencode's instructions (a headless flow's device code) or a token.
class OpenCodeSignIn : public ISignIn {
    Q_OBJECT
public:
    // One provider of the allow-list: opencode's id for it and the name shown.
    struct Provider {
        QString id;
        QString name;
    };

    explicit OpenCodeSignIn(std::shared_ptr<OpenCodeServer> server, QObject *parent = nullptr);
    ~OpenCodeSignIn() override;

    bool busy() const override { return op_ != Op::Idle; }
    // True from just before linkReady() until the operation ends, when the sign-in just authorized is a
    // "code" flow: the browser ends at a code to paste back, so the caller shows a code field and calls
    // submitCode(). (The fact is also added to the instructions text. Which flow a method is only becomes
    // known at authorize, so SignInMethod::needsCode stays false.)
    bool pendingNeedsCode() const override { return needsCode_; }

    // --- pure, tested ---
    // The allow-list: the sign-ins opencode itself provides (read from a sandboxed opencode 1.19.3), in the
    // order they are offered.
    static const QList<Provider> &providers();
    // The OAuth entries of an allow-listed provider that ask no questions, as methods "<provider>#<index>",
    // in the allow-list's order; `listed` is opencode's own list (OpenCodeWire::parseAuthMethods).
    static QList<SignInMethod> offeredMethods(const QList<OpenCodeAuthMethod> &listed);
    // "<provider>#<index>" -> its parts. The provider is not checked against the allow-list.
    static bool parseMethodId(const QString &id, QString *providerId, int *index);
    // Signed in when any allow-listed provider is among the `connected` ones; the detail names them.
    static SignInStatus statusFor(const QStringList &connected);

public slots:
    // statusChecked, or — the server did not start, or answered nonsense — statusChecked(Unknown) and failed.
    void checkStatus() override;
    // methodsListed | failed.
    void listMethods() override;
    // linkReady, then signedIn | failed. `methodId` must be one of listMethods()'s: with several to choose
    // from, an empty one fails.
    void signIn(const QString &methodId = {}) override;
    // For a pending "code" flow only; anything else fails (and leaves a running operation alone).
    void submitCode(const QString &code) override;
    // Removes the stored login of every allow-listed provider that is connected — opencode's `auth.json`, so
    // the terminal opencode loses it too, an API key stored for that provider included — then signedOut() when
    // none is connected any more, else failed() naming the provider that stays (as one set in the environment).
    void signOut() override;
    void cancel() override;

private:
    enum class Op { Idle, Listing, Checking, Authorizing, WaitingForBrowser, WaitingForCode, SubmittingCode,
                    Refreshing, SigningOut };
    // What a POST answered: its HTTP status and body, or why there was none.
    struct Reply {
        int status{0};
        QByteArray body;
        std::optional<Error> transport;
    };

    quint64 begin(Op op);                       // 0, and failed(), when busy
    void end();                                 // idle again; whatever is still on its way is ignored
    void fail(const Error &error);
    bool current(quint64 serial) const { return serial == serial_ && op_ != Op::Idle; }

    void whenStarted(quint64 serial, std::function<void(Result<void>)> next);
    void get(quint64 serial, const QString &path, std::function<void(Result<QByteArray>)> next);
    void post(quint64 serial, const QString &path, const QByteArray &body, int idleTimeoutMs,
              std::function<void(Reply)> next);
    void remove(quint64 serial, const QString &path, std::function<void(Result<QByteArray>)> next);
    void fetchConnected(quint64 serial, std::function<void(Result<QStringList>)> next);
    void disposeThenCheck(quint64 serial, std::function<void(Result<QStringList>)> next);
    void leaveMcpOut(quint64 serial, std::function<void()> next);
    void disconnectMcp(const QStringList &names, std::function<void()> done);

    void authorize(quint64 serial, const QString &providerId, int index);
    void waitForBrowser(quint64 serial, const QString &providerId, int index);
    void finishSignIn(quint64 serial, const QString &providerId);
    void removeLogins(quint64 serial, QStringList targets, qsizetype at);

    std::shared_ptr<OpenCodeServer> server_;
    Op op_{Op::Idle};
    quint64 serial_{0};
    QPointer<QNetworkReply> reply_;             // what the operation waits for, so cancel() can end it
    bool needsCode_{false};
    QString pendingProvider_;                   // the flow waiting for a code
    int pendingIndex_{0};
};

} // namespace nexa

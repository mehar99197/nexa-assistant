#pragma once

#include "core/Result.h"
#include "core/ai/SignIn.h"

#include <QList>
#include <QString>
#include <QUrl>

namespace nexa {

// What Settings shows of one program's sign-in: the status line, which buttons work, the link and the code
// field. Plain data, so the row's logic is testable without a widget.
struct SignInView {
    enum class Tone { Neutral, Good, Warn, Bad };   // the sheet colours it; the words carry the meaning too

    QString statusText;
    Tone tone{Tone::Neutral};
    bool busy{false};               // a call to the program is running: every button but Cancel is off
    bool canSignIn{false};
    bool canSignOut{false};
    bool canCancel{false};
    bool showCode{false};           // the browser ends at a code to paste back
    QString link;                   // empty: none to show (agy opens the browser itself); always selectable text
    QString linkHint;               // the sentence above the link, with the program's own words when it has some
    QList<SignInMethod> methods;    // what listMethods() offered
};

// View-model (state machine) for the sign-in row of a program-backed provider in Settings → Providers. The
// widget feeds it what the ISignIn says and what the user does, and draws `view()`; nothing here touches Qt
// Widgets, a program or the network. Texts follow the design (§4): never an email, a code, a token or a link in
// a status line, and no `Error::message` is ever shown (it may hold a URL) — only a sentence per ErrorCode.
//
// The refused call: an ISignIn that is busy answers a second call with failed() while the first goes on. The
// widget passes `stillBusy = signIn->busy()` read in the failed() slot; a failure with `stillBusy` true is such
// a refusal and changes nothing, so it can never end a running sign-in.
class SignInPresenter {
public:
    // `kind` is kKindAgy, kKindOpenCode, kKindCodex or kKindClaudeCode: it decides the keyring wording and what sign-out
    // says.
    explicit SignInPresenter(QString kind);

    const QString &kind() const { return kind_; }
    bool busy() const { return op_ != Op::None; }
    const SignInView &view() const { return view_; }
    // The confirmation to ask before signing out, for the last status this presenter saw.
    QString signOutWarning() const { return signOutWarning(kind_, detail_); }

    // --- what the user did ---
    void checkStarted();
    void listStarted();
    void signInStarted(const QString &methodId = {});
    void codeSubmitted();
    void signOutStarted();

    // --- what the ISignIn said ---
    void statusChecked(const SignInStatus &status);
    void checkFailed(const Error &error);
    void methodsListed(const QList<SignInMethod> &methods);
    // The program listed no browser sign-in it offers (opencode, when none of its allow-listed providers has one):
    // said instead of sending a blank method the program can only refuse.
    void noMethodsOffered();
    // `needsCode`: ISignIn::pendingNeedsCode() as the signal arrives (opencode's "code" flow).
    void linkReady(const QUrl &url, const QString &instructions, bool browserOpened, bool needsCode = false);
    void signedIn(const SignInStatus &status);
    void signedOut();
    void failed(const Error &error, bool stillBusy);
    void cancelled();

    // --- pure texts ---
    // One sentence per code, for a sign-in or sign-out that failed; `forCheck` for a status check that did.
    static QString failureText(ErrorCode code, bool forCheck = false);
    // The question before signing out; says what else is signed out with it. `detail` is the status detail.
    static QString signOutWarning(const QString &kind, const QString &detail);

private:
    enum class Op { None, Checking, Listing, SigningIn, SigningOut };

    void setText(const QString &text, SignInView::Tone tone);
    void endOp();
    void clearLink();
    void refresh();                 // the buttons, from the state

    QString kind_;
    SignInView view_;
    Op op_{Op::None};
    SignInStatus::State state_{SignInStatus::State::Unknown};
    QString detail_;
    QString methodId_;
    bool signOutSupported_{true};
    bool codeNeeded_{false};
    bool codeSent_{false};
};

} // namespace nexa

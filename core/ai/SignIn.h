#pragma once

#include "core/Result.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QUrl>

namespace nexa {

// Where a program Nexa runs (agy, opencode, codex) stands with its own sign-in. `detail` is the plan or
// account kind ("Google AI Pro", "ChatGPT · Free") — never an email, a token or a code.
struct SignInStatus {
    enum class State { Unknown, SignedOut, SignedIn };
    State state{State::Unknown};
    QString detail;
};

// One way to sign in: agy has one, opencode one per provider it offers an OAuth login for.
struct SignInMethod {
    QString id;
    QString label;
    bool needsCode{false};  // the browser ends at a page that shows a code to paste back (submitCode)
};

// Strategy: how a program is signed in and out from inside Nexa. The vendor's own program does the OAuth —
// Nexa starts it, shows its login link and reads its status — so a token never passes through Nexa. One
// operation at a time; cancel() ends the program and fails the operation with ErrorCode::Cancelled.
class ISignIn : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual bool busy() const = 0;
    // True while a sign-in that was started is a "code" flow: the browser ends at a code the user pastes back,
    // so the caller shows a code field and passes it to submitCode(). Only opencode has such flows.
    virtual bool pendingNeedsCode() const { return false; }

public slots:
    virtual void checkStatus() = 0;                         // -> statusChecked | failed
    virtual void listMethods() = 0;                         // -> methodsListed | failed
    virtual void signIn(const QString &methodId = {}) = 0;  // -> linkReady, then signedIn | failed
    virtual void submitCode(const QString &code) = 0;       // only for a method with needsCode
    virtual void signOut() = 0;                             // -> signedOut | failed
    virtual void cancel() = 0;

signals:
    void statusChecked(nexa::SignInStatus status);
    void methodsListed(QList<nexa::SignInMethod> methods);
    // The link to sign in at. `browserOpened`: the program already opened it; otherwise the caller does.
    void linkReady(QUrl url, QString instructions, bool browserOpened);
    void signedIn(nexa::SignInStatus status);
    void signedOut();
    void failed(nexa::Error error);
};

} // namespace nexa

Q_DECLARE_METATYPE(nexa::SignInStatus)
Q_DECLARE_METATYPE(nexa::SignInMethod)
Q_DECLARE_METATYPE(QList<nexa::SignInMethod>)

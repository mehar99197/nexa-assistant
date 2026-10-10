#pragma once

#include <QObject>
#include <QString>

#include <deque>
#include <functional>

namespace nexa {

// Asks polkit whether a caller may perform an action, before the helper does anything privileged.
//
// Who the caller is comes from the bus, never from the message body: the helper hands over the
// caller's unique D-Bus name and the bus resolves it to the process behind it, so a program cannot
// claim to be another one.
//
// What polkit remembers, and what it does not. The action is auth_admin_keep, so polkit asks for a
// password once and then remembers the answer — but against the *process* that asked, which for
// Nexa is the wrong lifetime: every new Nexa process is a stranger to it and gets a fresh prompt.
// Asking polkit about the login session instead would have the right lifetime, but polkitd 127
// aborts when asked that, and a crash loses everything it was remembering. So the answer is kept
// by the helper instead (see SessionGrants) and polkit is asked once per login.
//
// The password is typed into the desktop's own authentication agent, never into Nexa, so it never
// passes through this code.
//
// polkit-qt's Authority reports results through one signal with no request id, so checks run
// strictly one at a time and queue behind each other.
class PolkitAuthorizer : public QObject {
    Q_OBJECT
public:
    using Answer = std::function<void(bool allowed, const QString &reason)>;

    explicit PolkitAuthorizer(QObject *parent = nullptr);

    void check(const QString &actionId, const QString &callerBusName, Answer answer);

    // Drops whatever polkit is still remembering for this caller, so giving up an authorisation
    // means it on both sides.
    void revoke(const QString &callerBusName);

private:
    struct Request {
        QString actionId;
        QString callerBusName;
        Answer answer;
    };

    void startNext();
    void finish(bool allowed, const QString &reason);

    std::deque<Request> queue_;
    bool busy_{false};
};

} // namespace nexa

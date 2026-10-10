#pragma once

#include <QObject>
#include <QString>

#include <deque>
#include <functional>

namespace nexa {

// Asks polkit whether a caller may perform an action, before the helper does anything privileged.
//
// Who the caller is comes from the bus, never from the message body: the helper hands over the
// caller's unique D-Bus name and the bus resolves it to a process id, so a program cannot claim to
// be another one. That process id is then turned into the **login session** it belongs to, and the
// session is what polkit is asked about.
//
// Both Nexa actions are auth_admin_keep, so polkit asks for a password once and then remembers it
// for the rest of the login session — the bus name is enough for that, because polkit resolves it
// to a session itself. Asking about the session directly instead crashes polkitd 127, and a crash
// loses everything it was remembering.
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

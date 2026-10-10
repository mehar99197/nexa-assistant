#pragma once

#include "helper/LoginSession.h"

#include <QDBusConnection>

namespace nexa {

// Adapter over systemd-logind: answers "which graphical login does this user id have, and is it
// the one on the screen right now?".
//
// The user id is the one the bus reports for the calling connection — the kernel told the bus who
// connected, so a program cannot claim to be somebody else — and everything else is read from
// logind. Nothing here comes out of a D-Bus message body.
//
// The user's *display* session is the one asked about, which is the same session polkit falls back
// to when the calling process has no session of its own (a program started from a terminal outside
// the login, for instance). That keeps this agreeing with polkit's own answer.
class LoginSessions {
public:
    explicit LoginSessions(const QDBusConnection &bus);

    // The user's graphical session, or an invalid LoginSession when they have none.
    LoginSession displaySessionOf(uint uid) const;

private:
    QDBusConnection bus_;
};

} // namespace nexa

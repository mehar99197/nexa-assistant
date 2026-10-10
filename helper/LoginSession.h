#pragma once

#include <QString>

namespace nexa {

// The graphical login a caller belongs to, as logind describes it.
//
// This is what an authorisation is remembered against. It begins when the user logs in and ends
// when they log out, so "ask for the password once per login" means "ask once per one of these".
// Deliberately not the calling process: Nexa is restarted often, and polkit's own "remember this"
// is tied to the process that asked, which is why the password kept coming back.
struct LoginSession {
    uint uid{0};                            // from the bus, never from the caller
    QString id;                             // logind session id, e.g. "2"
    qulonglong startedMonotonicUsec{0};     // so a reused id after a logout is not mistaken for this login
    bool active{false};                     // this is the session on the screen right now
    bool remote{false};                     // true for ssh and the like

    bool isValid() const { return !id.isEmpty(); }

    // Only the person sitting in front of the machine may authorise anything, which is the same
    // rule as allow_inactive=no and allow_any=no in packaging/org.nexa.helper.policy. Checked on
    // every privileged call, not just when the password was asked for: switching to another user's
    // session has to take the privilege away again.
    bool isAtTheScreen() const { return isValid() && active && !remote; }
};

} // namespace nexa

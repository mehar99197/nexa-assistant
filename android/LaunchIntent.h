#pragma once

#include <QString>

namespace nexa {

// What the PC handed the app when it started it over adb (app/PhoneLink): where Nexa is and the
// pairing code, so the app pairs by itself. On Android these are the launch intent's extras
// (nexa.address, nexa.code, nexa.name), which only this app receives; on the desktop the command
// line carries them, for trying the same thing by hand.
struct LaunchIntent {
    QString address;    // "localhost:7777" through adb, or an https address on the Wi-Fi
    QString code;       // the six digits
    QString name;       // what the PC will list this phone as (optional)

    bool isPairing() const { return !address.isEmpty() && !code.isEmpty(); }

    // The activity's intent on Android; empty elsewhere.
    static LaunchIntent fromActivity();
};

} // namespace nexa

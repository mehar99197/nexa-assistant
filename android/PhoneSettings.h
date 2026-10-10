#pragma once

#include "remote/RemoteClient.h"

#include <QSettings>
#include <QString>

namespace nexa {

// What the phone remembers between runs: where Nexa is, the token pairing handed out, the
// certificate fingerprint the user trusted, and the name this phone gave itself (Repository over
// QSettings — on Android that is the app's private storage; nothing in it leaves the phone).
class PhoneSettings {
public:
    // The app's own store, or a file of its own (the tests').
    explicit PhoneSettings(const QString &file = {});

    RemoteEndpoint endpoint() const;
    QString token() const;
    QString fingerprint() const;
    bool isPaired() const { return endpoint().isValid() && !token().isEmpty(); }
    // The name the PC lists this phone under: the model, unless the user chose one.
    QString deviceName() const;

    void save(const RemoteEndpoint &endpoint, const QString &token, const QString &fingerprint);
    void setDeviceName(const QString &name);
    // Unpaired: the token goes, the address stays for the next time.
    void forgetToken();

private:
    QSettings settings_;
};

} // namespace nexa

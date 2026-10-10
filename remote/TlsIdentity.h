#pragma once

#include "core/Result.h"

#include <QObject>
#include <QSslConfiguration>
#include <QString>
#include <QStringList>

#include <functional>

namespace nexa {

// The web remote's own certificate: self-signed, made once with openssl and kept in
// ~/.config/nexa/tls (the key readable by the user alone), and made again only when it expires —
// every new certificate means another "your connection is not private" page on the phone.
//
// Self-signed means the browser cannot vouch for it, so Settings → Remote shows its fingerprint:
// the one the browser displays should match. The phone can also skip TLS altogether by using
// http://localhost through `adb reverse`, which travels over the adb link instead.
class TlsIdentity : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<QSslConfiguration>)>;

    explicit TlsIdentity(QString directory, QObject *parent = nullptr);
    static QString defaultDirectory();

    // Loads the certificate, making one first when there is none or it has expired. `addresses`
    // are the IPs the phone may use, written into the certificate so it matches them.
    void ensure(const QStringList &addresses, Done done);
    // SHA-256 of the certificate as the browser shows it ("AB:CD:…"); empty before ensure().
    QString fingerprint() const { return fingerprint_; }

private:
    Result<QSslConfiguration> load();

    QString directory_;
    QString fingerprint_;
};

} // namespace nexa

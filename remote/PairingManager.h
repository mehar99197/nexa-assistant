#pragma once

#include "core/Result.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace nexa {

// A browser allowed to use the web remote — normally the user's phone.
struct PairedDevice {
    QString id;                 // short and random, for revoking
    QString name;               // what the browser called itself, e.g. "Chrome on Android"
    QString tokenHash;          // SHA-256 of the token, hex: the token itself is never stored
    QDateTime paired;
    QDateTime lastSeen;
};

// Who may use the web remote. A pairing code shown on the PC — six digits, good for five minutes
// and for one pairing — is traded for a long random token the phone keeps. Only a hash of the token
// is stored, so the file on disk lets nobody in. Wrong codes count against the code, which is
// withdrawn after a few, so it cannot be guessed within its lifetime.
class PairingManager : public QObject {
    Q_OBJECT
public:
    using Clock = std::function<QDateTime()>;
    static constexpr int kCodeLifetimeSeconds = 5 * 60;
    static constexpr int kMaxCodeAttempts = 10;

    // `clock` is for tests; the real one is used when it is empty.
    explicit PairingManager(QString path, Clock clock = {}, QObject *parent = nullptr);
    static QString defaultPath();       // ~/.config/nexa/remote-devices.json

    // A fresh code, replacing any earlier one.
    QString startPairing();
    void stopPairing();
    QString code() const;               // empty when no code is valid right now
    QDateTime codeExpires() const { return codeExpires_; }

    struct Paired {
        QString token;                  // handed to the phone once, never kept here
        PairedDevice device;
    };
    Result<Paired> pair(const QString &code, const QString &deviceName);

    std::optional<PairedDevice> authenticate(const QString &token);
    bool revoke(const QString &id);
    QList<PairedDevice> devices() const { return devices_; }

    static QString hashToken(const QString &token);

signals:
    void devicesChanged();
    void revoked(const QString &id);            // that device's sessions must end now
    void codeChanged(const QString &code);      // empty once the code is spent, withdrawn or stopped

private:
    QDateTime now() const;
    void load();
    void save() const;

    QString path_;
    Clock clock_;
    QList<PairedDevice> devices_;
    QString code_;
    QDateTime codeExpires_;
    int attempts_{0};
};

} // namespace nexa

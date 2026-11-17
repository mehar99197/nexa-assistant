#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace nexa::licensetoken {

// Signed claims carried inside the server's Ed25519 JWT license token.
struct Claims {
    QString licenseKey;     // "sub"
    QString plan;           // "free" | "pro" | "team"
    QString product;        // must be "assistant"
    QString device;         // device fingerprint
    QString account;        // "acct" (account id string or empty)
    QDateTime issuedAt;     // "iat" UTC
    QDateTime expiresAt;    // "exp" UTC
    QJsonObject features;   // signed entitlements
};

// Verifies an Ed25519 ("EdDSA") JWT against the embedded public key,
// demanding typ=="license", product=="assistant", valid exp, and known plan.
// Does NOT enforce expiry against now (offlineGraceAllows checks grace window).
[[nodiscard]] bool verify(const QString &token, Claims *out, QString *error = nullptr);

// True if verified `claims` keep a paid plan alive offline on `deviceFingerprint`.
[[nodiscard]] bool offlineGraceAllows(const Claims &claims, const QString &deviceFingerprint,
                                      const QDateTime &now, int graceDays);

// Verifies a detached Ed25519 signature (raw 64 bytes) with the vendor public key.
[[nodiscard]] bool verifyDetachedSignature(const QByteArray &message, const QByteArray &signature);

[[nodiscard]] QByteArray publicKeyHex();
[[nodiscard]] bool usingDevelopmentKey();

} // namespace nexa::licensetoken

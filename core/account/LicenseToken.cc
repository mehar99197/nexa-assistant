#include "core/account/LicenseToken.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QTimeZone>

#include <openssl/evp.h>

using namespace Qt::StringLiterals;

namespace nexa::licensetoken {

namespace {

#ifdef NEXA_LICENSE_PUBLIC_KEY
constexpr auto kPublicKeyHex = NEXA_LICENSE_PUBLIC_KEY;
constexpr bool kIsDevelopmentKey = false;
#else
constexpr auto kPublicKeyHex = "8e486e8fb79499dba2a789c6b1639d409495233c79e0eaf52e2ea1fc9dbbb73f";
constexpr bool kIsDevelopmentKey = true;
#endif

constexpr int kEd25519PublicKeyBytes = 32;
constexpr int kEd25519SignatureBytes = 64;
constexpr qsizetype kMaxTokenChars = 8 * 1024;

bool fail(QString *error, const char *why) {
    if (error) *error = QString::fromLatin1(why);
    return false;
}

bool decodeSegment(const QByteArray &segment, QByteArray *out) {
    QByteArray padded = segment;
    switch (padded.size() % 4) {
    case 2: padded += "=="; break;
    case 3: padded += "="; break;
    case 1: return false;
    default: break;
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        padded, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return false;
    *out = *decoded;
    return true;
}

bool parseJsonObject(const QByteArray &bytes, QJsonObject *out) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
    *out = document.object();
    return true;
}

bool verifySignature(const QByteArray &signingInput, const QByteArray &signature) {
    const QByteArray key = QByteArray::fromHex(QByteArray(kPublicKeyHex));
    if (key.size() != kEd25519PublicKeyBytes) return false;
    if (signature.size() != kEd25519SignatureBytes) return false;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), size_t(key.size()));
    if (!pkey) return false;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    const bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1
        && EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(signature.constData()), size_t(signature.size()),
                            reinterpret_cast<const unsigned char *>(signingInput.constData()),
                            size_t(signingInput.size())) == 1;

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

QDateTime timeFromClaim(const QJsonValue &value) {
    if (!value.isDouble()) return {};
    return QDateTime::fromSecsSinceEpoch(qint64(value.toDouble()), QTimeZone::utc());
}

} // namespace

bool verifyDetachedSignature(const QByteArray &message, const QByteArray &signature) {
    return verifySignature(message, signature);
}

bool offlineGraceAllows(const Claims &claims, const QString &deviceFingerprint, const QDateTime &now, int graceDays) {
    if (claims.product != u"assistant") return false;
    if (claims.plan != u"pro" && claims.plan != u"team") return false;
    if (claims.device.isEmpty() || claims.device != deviceFingerprint) return false;
    if (!claims.issuedAt.isValid()) return false;
    if (claims.issuedAt > now.addSecs(300)) return false;
    return claims.issuedAt.daysTo(now) <= graceDays;
}

QByteArray publicKeyHex() {
    return QByteArray(kPublicKeyHex);
}

bool usingDevelopmentKey() {
    return kIsDevelopmentKey;
}

bool verify(const QString &token, Claims *out, QString *error) {
    if (!out) return fail(error, "no output");
    if (token.isEmpty() || token.size() > kMaxTokenChars) return fail(error, "empty or oversized token");

    const QByteArray raw = token.toLatin1();
    const QList<QByteArray> parts = raw.split('.');
    if (parts.size() != 3 || parts[0].isEmpty() || parts[1].isEmpty() || parts[2].isEmpty()) {
        return fail(error, "malformed token");
    }

    QByteArray headerBytes;
    if (!decodeSegment(parts[0], &headerBytes)) return fail(error, "malformed header");
    QJsonObject header;
    if (!parseJsonObject(headerBytes, &header)) return fail(error, "malformed header");
    if (header.value(u"alg"_s).toString() != u"EdDSA"_s) return fail(error, "unexpected algorithm");

    QByteArray signature;
    if (!decodeSegment(parts[2], &signature)) return fail(error, "malformed signature");

    const QByteArray signingInput = parts[0] + '.' + parts[1];
    if (!verifySignature(signingInput, signature)) return fail(error, "signature does not verify");

    QByteArray payloadBytes;
    if (!decodeSegment(parts[1], &payloadBytes)) return fail(error, "malformed payload");
    QJsonObject payload;
    if (!parseJsonObject(payloadBytes, &payload)) return fail(error, "malformed payload");

    if (payload.value(u"typ"_s).toString() != u"license"_s) return fail(error, "not a licence token");
    const QString product = payload.value(u"product"_s).toString();
    if (product != u"assistant"_s) return fail(error, "wrong product");

    const QDateTime expires = timeFromClaim(payload.value(u"exp"_s));
    if (!expires.isValid()) return fail(error, "missing exp");

    Claims claims;
    claims.licenseKey = payload.value(u"sub"_s).toString();
    claims.plan = payload.value(u"plan"_s).toString();
    claims.product = product;
    claims.device = payload.value(u"device"_s).toString();
    const QJsonValue account = payload.value(u"acct"_s);
    if (account.isDouble()) {
        claims.account = QString::number(qint64(account.toDouble()));
    } else if (account.isString()) {
        claims.account = account.toString();
    }
    claims.issuedAt = timeFromClaim(payload.value(u"iat"_s));
    claims.expiresAt = expires;
    claims.features = payload.value(u"features"_s).toObject();

    if (claims.plan != u"free"_s && claims.plan != u"pro"_s && claims.plan != u"team"_s) {
        return fail(error, "unknown plan");
    }

    *out = claims;
    return true;
}

} // namespace nexa::licensetoken

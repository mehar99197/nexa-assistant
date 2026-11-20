#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <openssl/evp.h>

namespace nexa::test {

// Publicly known development Ed25519 seed (matches backend dev fallback & LicenseToken dev pubkey 8e486e8f...).
inline QByteArray devSeed() {
    return QByteArray(32, '\x4e');
}

inline QByteArray otherSeed() {
    return QByteArray(32, '\x5f');
}

inline QByteArray base64Url(const QByteArray &raw) {
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

inline QByteArray ed25519Sign(const QByteArray &message, const QByteArray &seed = devSeed()) {
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(seed.constData()), size_t(seed.size()));
    if (!pkey) return {};
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    QByteArray sig(64, '\0');
    size_t sigLen = size_t(sig.size());
    const bool ok = ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1
        && EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(sig.data()), &sigLen,
                          reinterpret_cast<const unsigned char *>(message.constData()), size_t(message.size())) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    if (!ok) return {};
    sig.resize(qsizetype(sigLen));
    return sig;
}

inline QString signJwt(const QJsonObject &header, const QJsonObject &payload, const QByteArray &seed = devSeed()) {
    const QByteArray signingInput = base64Url(QJsonDocument(header).toJson(QJsonDocument::Compact)) + '.'
        + base64Url(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QByteArray sig = ed25519Sign(signingInput, seed);
    return QString::fromLatin1(signingInput + '.' + base64Url(sig));
}

inline QString signAssistantJwt(const QString &plan, const QString &device, qint64 iat, qint64 exp,
                                const QJsonObject &features = {}, const QString &product = QStringLiteral("assistant"),
                                const QByteArray &seed = devSeed(), const QJsonValue &acct = QJsonValue()) {
    const QJsonObject header{{QStringLiteral("alg"), QStringLiteral("EdDSA")},
                             {QStringLiteral("typ"), QStringLiteral("JWT")}};
    QJsonObject payload{
        {QStringLiteral("typ"), QStringLiteral("license")},
        {QStringLiteral("sub"), QStringLiteral("NDM-AAAA-BBBB-CCCC")},
        {QStringLiteral("plan"), plan},
        {QStringLiteral("product"), product},
        {QStringLiteral("device"), device},
        {QStringLiteral("iat"), double(iat)},
        {QStringLiteral("exp"), double(exp)},
    };
    if (!acct.isNull() && !acct.isUndefined()) payload.insert(QStringLiteral("acct"), acct);
    if (!features.isEmpty()) payload.insert(QStringLiteral("features"), features);
    return signJwt(header, payload, seed);
}

inline QString signUpdateManifest(const QString &version, const QString &url, const QString &sha256,
                                  const QString &notes = QStringLiteral("Fixes"), const QByteArray &seed = devSeed()) {
    const QByteArray msg = "nexa-update-v1\n" + version.toUtf8() + '\n' + url.toUtf8() + '\n' + sha256.toUtf8();
    const QString sig = QString::fromLatin1(base64Url(ed25519Sign(msg, seed)));
    const QJsonObject obj{
        {QStringLiteral("version"), version},
        {QStringLiteral("url"), url},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("notes"), notes},
        {QStringLiteral("signature"), sig},
    };
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

} // namespace nexa::test

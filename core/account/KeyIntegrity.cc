#include "core/account/KeyIntegrity.h"
#include "core/account/LicenseToken.h"

#include <QByteArray>
#include <QCryptographicHash>

namespace nexa::licensetoken {

namespace {

#ifdef NEXA_LICENSE_KEY_DIGEST
constexpr auto kExpectedDigestHex = NEXA_LICENSE_KEY_DIGEST;
constexpr bool kDigestConfigured = true;
#else
constexpr auto kExpectedDigestHex = "";
constexpr bool kDigestConfigured = false;
#endif

} // namespace

bool publicKeyIntact() {
    if (!kDigestConfigured) return true;

    const QByteArray expected(kExpectedDigestHex);
    const QByteArray actual = QCryptographicHash::hash(publicKeyHex(), QCryptographicHash::Sha256).toHex();
    if (expected.size() != actual.size()) return false;

    unsigned char difference = 0;
    for (qsizetype i = 0; i < expected.size(); ++i) {
        difference |= static_cast<unsigned char>(expected.at(i) ^ actual.at(i));
    }
    return difference == 0;
}

} // namespace nexa::licensetoken

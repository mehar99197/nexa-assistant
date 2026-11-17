#pragma once

#include <QString>

namespace nexa::credentialstore {

QString readLicenseKey();
bool writeLicenseKey(const QString &key);
bool removeLicenseKey();

QString readAccountToken();
bool writeAccountToken(const QString &token);
bool removeAccountToken();

// Clears the in-process test store when built with NEXA_TEST_CREDENTIAL_STORE (no-op otherwise).
void clearForTesting();

} // namespace nexa::credentialstore

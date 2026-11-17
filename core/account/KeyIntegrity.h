#pragma once

namespace nexa::licensetoken {

// Constant-time verification in a separate TU that the compiled-in public key
// matches the build-time SHA-256 digest (NEXA_LICENSE_KEY_DIGEST). Always true
// in developer builds lacking a digest.
[[nodiscard]] bool publicKeyIntact();

} // namespace nexa::licensetoken

#pragma once

#include <QString>

namespace nexa::portable {

// Portable mode: keeps every config, data, memory, audit, TLS, voice and cache file under
// <exeDir>/NexaData instead of ~/.config or %APPDATA%. Enabled by placing `portable.txt` next
// to a writable executable directory (used by the Windows portable zip).
bool isPortable();

// Pure helper (unit-tested): true iff `appDir` contains `portable.txt` and is writable.
bool detectPortable(const QString &appDir);

// Overrides portable detection in tests so nothing touches build output directories.
void setRootForTesting(const QString &appDir);
void clearRootForTesting();

// Root `<appDir>/NexaData` when portable, else empty.
QString rootDir();

// Equivalent to QStandardPaths::GenericConfigLocation + "/nexa" (or <NexaData>/config in portable mode).
QString configDir();

// Equivalent to QStandardPaths::GenericDataLocation + "/nexa" (or <NexaData>/data in portable mode).
QString dataDir();

// Equivalent to QStandardPaths::GenericCacheLocation + "/nexa" (or <NexaData>/cache in portable mode).
QString cacheDir();

// Redirects QSettings to <NexaData>/settings/Nexa.ini when portable. Call once before any QSettings access.
void initialise();

} // namespace nexa::portable

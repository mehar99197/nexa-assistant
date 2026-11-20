#include "core/account/CredentialStore.h"

#include "core/config/Portable.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>

#ifdef Q_OS_WIN
#include <string>
#include <windows.h>
#include <wincred.h>
#endif

using namespace Qt::StringLiterals;

namespace nexa::credentialstore {

namespace {

constexpr auto kLicenseSlot = "license";
constexpr auto kAccountSlot = "account-token";

#ifdef NEXA_TEST_CREDENTIAL_STORE

QHash<QString, QString> &memoryStore() {
    static QHash<QString, QString> store;
    return store;
}

QString readSecret(const char *slot) {
    return memoryStore().value(QString::fromLatin1(slot));
}

bool writeSecret(const char *slot, const QString &value) {
    memoryStore().insert(QString::fromLatin1(slot), value);
    return true;
}

bool removeSecret(const char *slot) {
    memoryStore().remove(QString::fromLatin1(slot));
    return true;
}

#elif defined(Q_OS_WIN)

std::wstring credentialName(const char *slot) {
    const QString name = u"NexaAssistant/"_s
        + (QLatin1String(slot) == QLatin1String(kAccountSlot) ? u"AccountToken"_s : u"LicenseKey"_s);
    return name.toStdWString();
}

QString readSecret(const char *slot) {
    const std::wstring name = credentialName(slot);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return {};
    const QString value = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(credential->CredentialBlob),
        int(credential->CredentialBlobSize / sizeof(char16_t)));
    CredFree(credential);
    return value;
}

bool writeSecret(const char *slot, const QString &value) {
    std::wstring name = credentialName(slot);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = name.data();
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t *>(L"NexaAssistant");
    credential.CredentialBlobSize = DWORD(value.size() * sizeof(char16_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<ushort *>(value.utf16()));
    return CredWriteW(&credential, 0) != FALSE;
}

bool removeSecret(const char *slot) {
    const std::wstring name = credentialName(slot);
    return CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0) != FALSE || GetLastError() == ERROR_NOT_FOUND;
}

#else

constexpr QFileDevice::Permissions kPrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr QFileDevice::Permissions kPrivateDir =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;

QString fallbackPath() {
    return portable::configDir() + u"/credentials.json"_s;
}

QJsonObject loadFallback() {
    QFile file(fallbackPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return {};
    QFile::setPermissions(fallbackPath(), kPrivateFile);
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool saveFallback(const QJsonObject &obj) {
    const QString path = fallbackPath();
    if (obj.isEmpty()) {
        return !QFile::exists(path) || QFile::remove(path);
    }
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) return false;
    QFile::setPermissions(dir, kPrivateDir);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    if (!file.commit()) return false;
    return QFile::setPermissions(path, kPrivateFile);
}

// Primary: GNOME Keyring / SecretService via python3-secretstorage (`application: NexaAssistant`).
// Fallback when headless/no keyring: 0600 JSON in 0700 config dir.
constexpr char kScript[] = R"PY(
import secretstorage, sys
bus = secretstorage.dbus_init()
collection = secretstorage.get_default_collection(bus)
if collection.is_locked():
    collection.unlock()
op, slot = sys.argv[1], sys.argv[2]
attrs = {'application': 'NexaAssistant', 'account': slot}
label = 'Nexa Assistant ' + ('account token' if slot == 'account-token' else 'license')
items = list(collection.search_items(attrs))
if op == 'get':
    if items:
        sys.stdout.buffer.write(items[0].get_secret())
elif op == 'set':
    secret = sys.stdin.buffer.read()
    collection.create_item(label, attrs, secret, replace=True)
elif op == 'delete':
    for item in items:
        item.delete()
)PY";

bool runSecretService(const QString &operation, const char *slot, const QByteArray &input, QByteArray *output) {
    QProcess process;
    process.start(u"python3"_s, {u"-c"_s, QString::fromUtf8(kScript), operation, QString::fromLatin1(slot)});
    if (!process.waitForStarted(2000)) return false;
    if (!input.isEmpty()) process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return false;
    }
    if (output) *output = process.readAllStandardOutput();
    return true;
}

QString readSecret(const char *slot) {
    QByteArray output;
    if (runSecretService(u"get"_s, slot, {}, &output)) {
        const QString ss = QString::fromUtf8(output).trimmed();
        if (!ss.isEmpty()) return ss;
    }
    return loadFallback().value(QString::fromLatin1(slot)).toString();
}

bool writeSecret(const char *slot, const QString &value) {
    if (runSecretService(u"set"_s, slot, value.toUtf8(), nullptr)) {
        QJsonObject obj = loadFallback();
        if (obj.contains(QString::fromLatin1(slot))) {
            obj.remove(QString::fromLatin1(slot));
            saveFallback(obj);
        }
        return true;
    }
    QJsonObject obj = loadFallback();
    obj.insert(QString::fromLatin1(slot), value);
    return saveFallback(obj);
}

bool removeSecret(const char *slot) {
    runSecretService(u"delete"_s, slot, {}, nullptr);
    QJsonObject obj = loadFallback();
    obj.remove(QString::fromLatin1(slot));
    return saveFallback(obj);
}

#endif

} // namespace

QString readLicenseKey() { return readSecret(kLicenseSlot); }
bool writeLicenseKey(const QString &key) { return writeSecret(kLicenseSlot, key); }
bool removeLicenseKey() { return removeSecret(kLicenseSlot); }

QString readAccountToken() { return readSecret(kAccountSlot); }
bool writeAccountToken(const QString &token) { return writeSecret(kAccountSlot, token); }
bool removeAccountToken() { return removeSecret(kAccountSlot); }

void clearForTesting() {
#ifdef NEXA_TEST_CREDENTIAL_STORE
    memoryStore().clear();
#endif
}

} // namespace nexa::credentialstore

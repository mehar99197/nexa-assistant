#include "remote/PairingManager.h"

#include "core/Logging.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include "core/config/Portable.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kFileVersion = 1;
constexpr qsizetype kMaxNameLength = 60;

QByteArray randomBytes(qsizetype count) {
    QByteArray bytes(count, Qt::Uninitialized);
    // The system generator: the kernel's, suitable for secrets.
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), count / 4);
    return bytes;
}

QString cleanName(const QString &name) {
    QString cleaned;
    for (const QChar c : name) {
        if (c.isPrint()) cleaned += c;
    }
    cleaned = cleaned.simplified().left(kMaxNameLength);
    return cleaned.isEmpty() ? u"Phone"_s : cleaned;
}

} // namespace

PairingManager::PairingManager(QString path, Clock clock, QObject *parent)
    : QObject(parent), path_(std::move(path)), clock_(std::move(clock)) {
    load();
}

QString PairingManager::defaultPath() {
    return portable::configDir() + u"/remote-devices.json"_s;
}

QDateTime PairingManager::now() const {
    return clock_ ? clock_() : QDateTime::currentDateTimeUtc();
}

QString PairingManager::hashToken(const QString &token) {
    return QString::fromLatin1(QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString PairingManager::startPairing() {
    code_ = u"%1"_s.arg(QRandomGenerator::system()->bounded(1'000'000), 6, 10, QLatin1Char('0'));
    codeExpires_ = now().addSecs(kCodeLifetimeSeconds);
    attempts_ = 0;
    emit codeChanged(code_);
    return code_;
}

void PairingManager::stopPairing() {
    if (code_.isEmpty()) return;
    code_.clear();
    emit codeChanged({});
}

QString PairingManager::code() const {
    return !code_.isEmpty() && now() < codeExpires_ ? code_ : QString();
}

Result<PairingManager::Paired> PairingManager::pair(const QString &code, const QString &deviceName) {
    if (code_.isEmpty() || now() >= codeExpires_) {
        return Error{ErrorCode::PermissionDenied,
                     u"There is no pairing code right now. On the PC: Settings → Remote → Pair a phone."_s};
    }
    // Compared all the way through, so the time it takes says nothing about how close a guess was.
    const QString given = code.trimmed();
    bool same = given.size() == code_.size();
    for (qsizetype i = 0; i < code_.size(); ++i) same &= i < given.size() && given.at(i) == code_.at(i);
    if (!same) {
        if (++attempts_ >= kMaxCodeAttempts) {
            stopPairing();
            qCWarning(lcApp) << "remote: pairing code withdrawn after" << kMaxCodeAttempts << "wrong attempts";
            return Error{ErrorCode::PermissionDenied, u"Too many wrong codes. Start pairing again on the PC."_s};
        }
        return Error{ErrorCode::PermissionDenied, u"That code is not right."_s};
    }
    stopPairing();      // one code, one phone

    const QString token = QString::fromLatin1(
        randomBytes(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    PairedDevice device;
    device.id = QString::fromLatin1(randomBytes(8).toHex());
    device.name = cleanName(deviceName);
    device.tokenHash = hashToken(token);
    device.paired = now();
    device.lastSeen = device.paired;
    devices_.append(device);
    save();
    qCInfo(lcApp).noquote() << "remote: paired" << device.name;
    emit devicesChanged();
    return Paired{token, device};
}

std::optional<PairedDevice> PairingManager::authenticate(const QString &token) {
    if (token.isEmpty()) return std::nullopt;
    const QString hash = hashToken(token);
    for (PairedDevice &device : devices_) {
        if (device.tokenHash != hash) continue;
        device.lastSeen = now();
        save();
        return device;
    }
    return std::nullopt;
}

bool PairingManager::revoke(const QString &id) {
    const qsizetype removed = devices_.removeIf([&id](const PairedDevice &device) { return device.id == id; });
    if (removed == 0) return false;
    save();
    qCInfo(lcApp).noquote() << "remote: revoked" << id;
    emit revoked(id);
    emit devicesChanged();
    return true;
}

void PairingManager::load() {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(u"version"_s).toInt() != kFileVersion) return;
    static const QRegularExpression sha256(u"^[0-9a-f]{64}$"_s);
    for (const QJsonValue &value : root.value(u"devices"_s).toArray()) {
        const QJsonObject object = value.toObject();
        PairedDevice device;
        device.id = object.value(u"id"_s).toString();
        device.name = cleanName(object.value(u"name"_s).toString());
        device.tokenHash = object.value(u"tokenHash"_s).toString();
        device.paired = QDateTime::fromString(object.value(u"paired"_s).toString(), Qt::ISODate);
        device.lastSeen = QDateTime::fromString(object.value(u"lastSeen"_s).toString(), Qt::ISODate);
        // Half a record is not a device: a missing or malformed hash would match nothing anyway.
        if (device.id.isEmpty() || !sha256.match(device.tokenHash).hasMatch()) continue;
        devices_.append(device);
    }
}

// The file says who may connect, so it is the user's alone, like config.json.
void PairingManager::save() const {
    if (path_.isEmpty()) return;
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QJsonArray list;
    for (const PairedDevice &device : devices_) {
        list.append(QJsonObject{{u"id"_s, device.id},
                                {u"name"_s, device.name},
                                {u"tokenHash"_s, device.tokenHash},
                                {u"paired"_s, device.paired.toString(Qt::ISODate)},
                                {u"lastSeen"_s, device.lastSeen.toString(Qt::ISODate)}});
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcApp).noquote() << "remote: cannot save the paired devices:" << file.errorString();
        return;
    }
    file.write(QJsonDocument(QJsonObject{{u"version"_s, kFileVersion}, {u"devices"_s, list}}).toJson());
    if (file.commit()) QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

} // namespace nexa

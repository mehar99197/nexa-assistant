#include "helper/DeviceLock.h"

#include "core/Logging.h"
#include "helper/InputDevices.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kVersion = 1;

QString readLine(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}

} // namespace

DeviceLock::DeviceLock(QString classRoot, QString recordPath)
    : classRoot_(std::move(classRoot)), recordPath_(std::move(recordPath)) {}

QString DeviceLock::defaultClassRoot() { return u"/sys/class/input"_s; }

QString DeviceLock::defaultRecordPath() { return u"/run/nexa-helper/locked-input.json"_s; }

Result<QStringList> DeviceLock::lock(const QString &owner) {
    if (isLocked() && owner != owner_) {
        return Error{ErrorCode::PermissionDenied, u"Another Nexa holds the keyboard and mouse."_s};
    }
    const QDir root(classRoot_);
    QList<Device> found;
    for (const QString &sysfs : root.entryList({u"input*"_s}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const bool held = std::any_of(held_.cbegin(), held_.cend(), [&sysfs](const Device &d) { return d.sysfs == sysfs; });
        if (held) continue;
        const QString base = root.filePath(sysfs);
        const QString path = QFileInfo(base).canonicalFilePath();
        inputdevices::Caps caps;
        caps.name = readLine(base + u"/name"_s);
        caps.ev = readLine(base + u"/capabilities/ev"_s);
        caps.key = readLine(base + u"/capabilities/key"_s);
        caps.rel = readLine(base + u"/capabilities/rel"_s);
        caps.abs = readLine(base + u"/capabilities/abs"_s);
        caps.isVirtual = path.contains(u"/devices/virtual/"_s);
        if (inputdevices::classify(caps) == inputdevices::Kind::Other) continue;
        if (readLine(base + u"/inhibited"_s) != u"0"_s) continue;      // someone else's, or no inhibit at all
        found.append(Device{sysfs, caps.name, path});
    }
    std::sort(found.begin(), found.end(), [](const Device &a, const Device &b) {
        return a.sysfs.mid(5).toInt() < b.sysfs.mid(5).toInt();
    });
    if (found.isEmpty() && held_.isEmpty()) {
        return Error{ErrorCode::NotFound, u"No keyboard or pointer was found to lock."_s};
    }

    const bool first = held_.isEmpty();
    if (first) owner_ = owner;
    // Recorded before it is switched off: the record must never know less than the kernel.
    const QList<Device> before = held_;
    held_.append(found);
    if (!found.isEmpty() && !save()) {
        held_ = before;
        if (first) owner_.clear();
        return Error{ErrorCode::Io, u"The lock could not be recorded in %1, so nothing was locked."_s.arg(recordPath_)};
    }
    for (const Device &device : std::as_const(found)) {
        QString why;
        if (setInhibited(classRoot_, device, true, &why)) continue;
        unlock();       // all or nothing: a half-locked laptop is the worst of both
        return Error{ErrorCode::Io, u"%1 could not be switched off: %2"_s.arg(device.name, why)};
    }
    for (const Device &device : std::as_const(found)) qCInfo(lcApp).noquote() << "locked" << device.sysfs << device.name;

    QStringList names;
    for (const Device &device : std::as_const(held_)) names.append(device.name);
    return names;
}

void DeviceLock::unlock() {
    for (const Device &device : std::as_const(held_)) {
        if (!stillThere(classRoot_, device)) continue;      // unplugged meanwhile
        QString why;
        if (setInhibited(classRoot_, device, false, &why)) {
            qCInfo(lcApp).noquote() << "released" << device.sysfs << device.name;
        } else {
            qCWarning(lcApp).noquote() << "cannot release" << device.sysfs << device.name << ":" << why;
        }
    }
    held_.clear();
    owner_.clear();
    QFile::remove(recordPath_);
}

int DeviceLock::releaseRecorded(const QString &recordPath, const QString &classRoot) {
    const QList<Device> recorded = load(recordPath);
    int released = 0;
    for (const Device &device : recorded) {
        if (!stillThere(classRoot, device)) continue;
        QString why;
        if (setInhibited(classRoot, device, false, &why)) {
            ++released;
            qCInfo(lcApp).noquote() << "released" << device.sysfs << device.name << "from the record";
        } else {
            qCWarning(lcApp).noquote() << "cannot release" << device.sysfs << device.name << ":" << why;
        }
    }
    QFile::remove(recordPath);
    return released;
}

bool DeviceLock::setInhibited(const QString &classRoot, const Device &device, bool on, QString *why) {
    QFile file(classRoot + u"/"_s + device.sysfs + u"/inhibited"_s);
    // Unbuffered: the kernel's answer comes back from this write, not from a flush nobody checks.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        if (why) *why = file.errorString();
        return false;
    }
    if (file.write(on ? "1" : "0") != 1) {
        if (why) *why = file.errorString();
        return false;
    }
    return true;
}

bool DeviceLock::stillThere(const QString &classRoot, const Device &device) {
    return QFileInfo(classRoot + u"/"_s + device.sysfs).canonicalFilePath() == device.path;
}

QList<DeviceLock::Device> DeviceLock::load(const QString &recordPath) {
    QFile file(recordPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(u"version"_s).toInt() != kVersion) return {};
    QList<Device> devices;
    for (const QJsonValue &value : root.value(u"devices"_s).toArray()) {
        const QJsonObject object = value.toObject();
        const Device device{object.value(u"sysfs"_s).toString(), object.value(u"name"_s).toString(),
                            object.value(u"path"_s).toString()};
        // Only an "inputN" under the class directory: a record must not be able to point a write anywhere else.
        static const QRegularExpression valid(u"^input\\d+$"_s);
        if (valid.match(device.sysfs).hasMatch() && !device.path.isEmpty()) devices.append(device);
    }
    return devices;
}

bool DeviceLock::save() const {
    const QString directory = QFileInfo(recordPath_).absolutePath();
    if (!QDir().mkpath(directory)) {
        qCWarning(lcApp).noquote() << "cannot create" << directory;
        return false;
    }
    if (::chmod(QFile::encodeName(directory).constData(), 0700) != 0) {
        qCWarning(lcApp).noquote() << "cannot restrict" << directory << ":" << ::strerror(errno);
        return false;
    }
    QJsonArray devices;
    for (const Device &device : held_) {
        devices.append(QJsonObject{{u"sysfs"_s, device.sysfs}, {u"name"_s, device.name}, {u"path"_s, device.path}});
    }
    QSaveFile file(recordPath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(QJsonObject{{u"version"_s, kVersion}, {u"owner"_s, owner_}, {u"devices"_s, devices}})
                   .toJson(QJsonDocument::Indented));
    if (!file.commit()) return false;
    QFile::setPermissions(recordPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace nexa

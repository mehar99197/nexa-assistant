#include "helper/DeviceLock.h"

#include "core/Logging.h"
#include "helper/InputDevices.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>

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
    : classRoot_(std::move(classRoot)), recordPath_(std::move(recordPath)) {
    // What a record still names when the helper starts (releaseRecorded ran first) is stuck: keep trying it.
    stuck_ = load(recordPath_);
}

QString DeviceLock::defaultClassRoot() { return u"/sys/class/input"_s; }

QString DeviceLock::defaultRecordPath() { return u"/run/nexa-helper/locked-input.json"_s; }

Result<QStringList> DeviceLock::lock(const QString &owner) {
    if (isLocked() && owner != owner_) {
        return Error{ErrorCode::Unauthorized, u"Another Nexa holds the keyboard and mouse."_s};
    }
    const auto sameSysfs = [](const QString &sysfs) { return [&sysfs](const Device &d) { return d.sysfs == sysfs; }; };
    const QDir root(classRoot_);
    QList<Device> found;
    QList<Device> adopted;          // stuck from an earlier lock, and switched off still: this lock's now
    for (const QString &sysfs : root.entryList({u"input*"_s}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (std::any_of(held_.cbegin(), held_.cend(), sameSysfs(sysfs))) continue;
        const QString base = root.filePath(sysfs);
        const QString path = QFileInfo(base).canonicalFilePath();
        inputdevices::Caps caps;
        caps.name = readLine(base + u"/name"_s);
        caps.ev = readLine(base + u"/capabilities/ev"_s);
        caps.key = readLine(base + u"/capabilities/key"_s);
        caps.rel = readLine(base + u"/capabilities/rel"_s);
        caps.abs = readLine(base + u"/capabilities/abs"_s);
        caps.isVirtual = path.contains(u"/devices/virtual/input/"_s);      // uinput's: Nexa's own
        if (inputdevices::classify(caps) == inputdevices::Kind::Other) continue;
        const Device device{sysfs, caps.name, path};
        if (const auto stuck = std::find_if(stuck_.cbegin(), stuck_.cend(), sameSysfs(sysfs)); stuck != stuck_.cend()) {
            if (stuck->path == path) {
                adopted.append(device);
                continue;
            }
        }
        if (readLine(base + u"/inhibited"_s) != u"0"_s) continue;      // someone else's, or no inhibit at all
        found.append(device);
    }
    std::sort(found.begin(), found.end(), [](const Device &a, const Device &b) {
        return a.sysfs.mid(5).toInt() < b.sysfs.mid(5).toInt();
    });
    if (found.isEmpty() && adopted.isEmpty() && held_.isEmpty()) {
        return Error{ErrorCode::NotFound, u"No keyboard or pointer was found to lock."_s};
    }

    const bool first = held_.isEmpty();
    if (first) owner_ = owner;
    for (const Device &device : std::as_const(adopted)) {
        stuck_.removeIf([&device](const Device &d) { return d.sysfs == device.sysfs; });
        held_.append(device);
    }
    // Recorded before it is switched off: the record must never know less than the kernel.
    const QList<Device> before = held_;
    held_.append(found);
    if (!found.isEmpty() && !save()) {
        held_ = before;
        if (first && held_.isEmpty()) owner_.clear();
        return Error{ErrorCode::Io, u"The lock could not be recorded in %1, so nothing was locked."_s.arg(recordPath_)};
    }
    for (const Device &device : std::as_const(found)) {
        QString why;
        if (setInhibited(classRoot_, device, true, &why)) {
            qCInfo(lcApp).noquote() << "locked" << device.sysfs << device.name;
            continue;
        }
        if (first) {
            unlock();       // all or nothing: a half-locked laptop is the worst of both
            return Error{ErrorCode::Io, u"%1 could not be switched off: %2"_s.arg(device.name, why)};
        }
        // A device plugged in while locked that will not switch off: it stays working, and the lock holds.
        qCWarning(lcApp).noquote() << "cannot lock" << device.sysfs << device.name << ":" << why;
        held_.removeIf([&device](const Device &d) { return d.sysfs == device.sysfs; });
        save();
    }

    QStringList names;
    for (const Device &device : std::as_const(held_)) names.append(device.name);
    return names;
}

bool DeviceLock::unlock() {
    for (const Device &device : std::as_const(held_)) {
        if (!stillThere(classRoot_, device)) continue;      // unplugged meanwhile
        QString why;
        if (setInhibited(classRoot_, device, false, &why)) {
            qCInfo(lcApp).noquote() << "released" << device.sysfs << device.name;
        } else {
            qCWarning(lcApp).noquote() << "cannot release" << device.sysfs << device.name << ":" << why << "— trying again";
            stuck_.append(device);
        }
    }
    held_.clear();
    owner_.clear();
    save();
    return stuck_.isEmpty();
}

int DeviceLock::retryStuck() {
    const qsizetype before = stuck_.size();
    stuck_.removeIf([this](const Device &device) {
        if (!stillThere(classRoot_, device)) return true;   // unplugged: nothing left to give back
        if (!setInhibited(classRoot_, device, false)) return false;
        qCInfo(lcApp).noquote() << "released" << device.sysfs << device.name << "at last";
        return true;
    });
    if (stuck_.size() != before) save();
    return int(stuck_.size());
}

int DeviceLock::releaseRecorded(const QString &recordPath, const QString &classRoot) {
    QList<Device> left = load(recordPath);
    int released = 0;
    for (int attempt = 0; attempt < 5 && !left.isEmpty(); ++attempt) {
        if (attempt > 0) QThread::msleep(200);
        left.removeIf([&classRoot, &released](const Device &device) {
            if (!stillThere(classRoot, device)) return true;
            QString why;
            if (!setInhibited(classRoot, device, false, &why)) {
                qCWarning(lcApp).noquote() << "cannot release" << device.sysfs << device.name << ":" << why;
                return false;
            }
            ++released;
            qCInfo(lcApp).noquote() << "released" << device.sysfs << device.name << "from the record";
            return true;
        });
    }
    // What still will not come back stays recorded, for the next helper to keep trying.
    if (left.isEmpty()) {
        QFile::remove(recordPath);
    } else {
        write(recordPath, QString(), left);
    }
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
    if (held_.isEmpty() && stuck_.isEmpty()) {
        QFile::remove(recordPath_);
        return true;
    }
    return write(recordPath_, owner_, held_ + stuck_);
}

bool DeviceLock::write(const QString &recordPath, const QString &owner, const QList<Device> &devices) {
    const QString directory = QFileInfo(recordPath).absolutePath();
    if (!QDir().mkpath(directory)) {
        qCWarning(lcApp).noquote() << "cannot create" << directory;
        return false;
    }
    if (::chmod(QFile::encodeName(directory).constData(), 0700) != 0) {
        qCWarning(lcApp).noquote() << "cannot restrict" << directory << ":" << ::strerror(errno);
        return false;
    }
    QJsonArray list;
    for (const Device &device : devices) {
        list.append(QJsonObject{{u"sysfs"_s, device.sysfs}, {u"name"_s, device.name}, {u"path"_s, device.path}});
    }
    QSaveFile file(recordPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(QJsonObject{{u"version"_s, kVersion}, {u"owner"_s, owner}, {u"devices"_s, list}})
                   .toJson(QJsonDocument::Indented));
    if (!file.commit()) return false;
    QFile::setPermissions(recordPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace nexa

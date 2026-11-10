// helper/DeviceLock: hardcore mode's lock of the keyboard and pointer, over a fake /sys/class/input. What it must
// never do: lock the power button or Nexa's own devices, leave a device locked after unlock, release a device someone
// else switched off, or release a different device that took a gone one's place.

#include "helper/DeviceLock.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

const QString kKeyboardKeys = u"180000 20000 4000000000000020 0 0 10500f02140007 ff803078f900d401 feffffdfffcfffff "
                              "fffffffffffffffe"_s;

// A sysfs tree the way the kernel lays it out: the class directory holds links to where each device really is.
class FakeSys {
public:
    FakeSys() {
        add(u"input0"_s, u"devices/platform/i8042/serio0/input/input0"_s, u"AT Translated Set 2 keyboard"_s,
            u"120013"_s, kKeyboardKeys);
        add(u"input3"_s, u"devices/platform/PNP0C0C:00/input/input3"_s, u"Power Button"_s, u"3"_s,
            u"8000 10000000000000 0"_s);
        add(u"input6"_s, u"devices/platform/i8042/serio1/input/input6"_s, u"SynPS/2 Synaptics TouchPad"_s, u"b"_s,
            u"e520 10000 0 0 0 0"_s, u"0"_s, u"660800011000003"_s);
        add(u"input20"_s, u"devices/virtual/input/input20"_s, u"Nexa virtual keyboard"_s, u"120013"_s, kKeyboardKeys);
    }

    void add(const QString &sysfs, const QString &where, const QString &name, const QString &ev, const QString &key,
             const QString &rel = u"0"_s, const QString &abs = u"0"_s) {
        const QString path = dir_.filePath(where);
        QDir().mkpath(path + u"/capabilities"_s);
        write(path + u"/name"_s, name);
        write(path + u"/capabilities/ev"_s, ev);
        write(path + u"/capabilities/key"_s, key);
        write(path + u"/capabilities/rel"_s, rel);
        write(path + u"/capabilities/abs"_s, abs);
        write(path + u"/inhibited"_s, u"0"_s);
        QDir().mkpath(classRoot());
        QFile::link(u"../../"_s + where, classRoot() + u"/"_s + sysfs);
    }

    void remove(const QString &sysfs) {
        const QString link = classRoot() + u"/"_s + sysfs;
        QDir(QFileInfo(link).canonicalFilePath()).removeRecursively();
        QFile::remove(link);
    }

    QString inhibited(const QString &sysfs) const {
        QFile file(classRoot() + u"/"_s + sysfs + u"/inhibited"_s);
        if (!file.open(QIODevice::ReadOnly)) return u"gone"_s;
        return QString::fromLatin1(file.readAll()).trimmed();
    }
    void setInhibited(const QString &sysfs, const QString &value) {
        write(classRoot() + u"/"_s + sysfs + u"/inhibited"_s, value);
    }
    void rename(const QString &sysfs, const QString &name) { write(classRoot() + u"/"_s + sysfs + u"/name"_s, name); }

    QString classRoot() const { return dir_.filePath(u"class/input"_s); }
    QString record() const { return dir_.filePath(u"run/nexa-helper/locked-input.json"_s); }

private:
    static void write(const QString &path, const QString &text) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(text.toLatin1() + '\n');
    }

    QTemporaryDir dir_;
};

QStringList recordedNames(const QString &record) {
    QFile file(record);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QStringList names;
    for (const QJsonValue &device : QJsonDocument::fromJson(file.readAll()).object().value(u"devices"_s).toArray()) {
        names.append(device.toObject().value(u"sysfs"_s).toString());
    }
    return names;
}

} // namespace

TEST_CASE("Locking switches off the keyboard and the touchpad, and nothing else") {
    FakeSys sys;
    DeviceLock lock(sys.classRoot(), sys.record());
    const Result<QStringList> held = lock.lock(u":1.42"_s);
    REQUIRE(held);
    CHECK(held.value() == QStringList{u"AT Translated Set 2 keyboard"_s, u"SynPS/2 Synaptics TouchPad"_s});
    CHECK(lock.isLocked());
    CHECK(lock.owner() == u":1.42"_s);
    CHECK(sys.inhibited(u"input0"_s) == u"1"_s);
    CHECK(sys.inhibited(u"input6"_s) == u"1"_s);
    CHECK(sys.inhibited(u"input3"_s) == u"0"_s);       // the power button
    CHECK(sys.inhibited(u"input20"_s) == u"0"_s);      // Nexa's own
    CHECK(recordedNames(sys.record()) == QStringList{u"input0"_s, u"input6"_s});
    CHECK((QFile::permissions(sys.record()) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
}

TEST_CASE("Unlocking gives every device back and forgets the record") {
    FakeSys sys;
    DeviceLock lock(sys.classRoot(), sys.record());
    REQUIRE(lock.lock(u":1.42"_s));
    lock.unlock();
    CHECK_FALSE(lock.isLocked());
    CHECK(sys.inhibited(u"input0"_s) == u"0"_s);
    CHECK(sys.inhibited(u"input6"_s) == u"0"_s);
    CHECK_FALSE(QFile::exists(sys.record()));
    lock.unlock();      // twice is harmless
}

TEST_CASE("Locking again is the heartbeat, and locks a mouse plugged in since") {
    FakeSys sys;
    DeviceLock lock(sys.classRoot(), sys.record());
    REQUIRE(lock.lock(u":1.42"_s));
    sys.add(u"input30"_s, u"devices/pci0000:00/usb1/1-2/input/input30"_s, u"Logitech USB Optical Mouse"_s, u"17"_s,
            u"ff0000 0 0 0 0"_s, u"1943"_s);
    const Result<QStringList> again = lock.lock(u":1.42"_s);
    REQUIRE(again);
    CHECK(again.value().contains(u"Logitech USB Optical Mouse"_s));
    CHECK(sys.inhibited(u"input30"_s) == u"1"_s);
    CHECK(recordedNames(sys.record()) == QStringList{u"input0"_s, u"input6"_s, u"input30"_s});
    lock.unlock();
    CHECK(sys.inhibited(u"input30"_s) == u"0"_s);
}

TEST_CASE("A second Nexa cannot take the lock another one holds") {
    FakeSys sys;
    DeviceLock lock(sys.classRoot(), sys.record());
    REQUIRE(lock.lock(u":1.42"_s));
    const Result<QStringList> other = lock.lock(u":1.99"_s);
    REQUIRE_FALSE(other);
    CHECK(other.error().code == ErrorCode::PermissionDenied);
    CHECK(lock.owner() == u":1.42"_s);
    CHECK(sys.inhibited(u"input0"_s) == u"1"_s);
}

TEST_CASE("A device someone else switched off is left as it was") {
    FakeSys sys;
    sys.setInhibited(u"input6"_s, u"1"_s);      // e.g. a tablet mode switch did it
    DeviceLock lock(sys.classRoot(), sys.record());
    const Result<QStringList> held = lock.lock(u":1.42"_s);
    REQUIRE(held);
    CHECK(held.value() == QStringList{u"AT Translated Set 2 keyboard"_s});
    lock.unlock();
    CHECK(sys.inhibited(u"input6"_s) == u"1"_s);
}

TEST_CASE("Nothing to lock is said, not pretended") {
    FakeSys sys;
    sys.remove(u"input0"_s);
    sys.remove(u"input6"_s);
    DeviceLock lock(sys.classRoot(), sys.record());
    const Result<QStringList> held = lock.lock(u":1.42"_s);
    REQUIRE_FALSE(held);
    CHECK(held.error().code == ErrorCode::NotFound);
    CHECK_FALSE(lock.isLocked());
}

TEST_CASE("After a crash, the record is enough to give the devices back") {
    FakeSys sys;
    {
        DeviceLock lock(sys.classRoot(), sys.record());
        REQUIRE(lock.lock(u":1.42"_s));
        // The helper dies here: its destructor is not what releases them.
    }
    CHECK(sys.inhibited(u"input0"_s) == u"1"_s);
    CHECK(DeviceLock::releaseRecorded(sys.record(), sys.classRoot()) == 2);
    CHECK(sys.inhibited(u"input0"_s) == u"0"_s);
    CHECK(sys.inhibited(u"input6"_s) == u"0"_s);
    CHECK_FALSE(QFile::exists(sys.record()));
    CHECK(DeviceLock::releaseRecorded(sys.record(), sys.classRoot()) == 0);     // no record: nothing to do
}

TEST_CASE("A device that is no longer the one recorded is not touched") {
    FakeSys sys;
    DeviceLock lock(sys.classRoot(), sys.record());
    REQUIRE(lock.lock(u":1.42"_s));
    sys.remove(u"input6"_s);                         // the touchpad went…
    sys.add(u"input6"_s, u"devices/platform/other/input/input6"_s, u"Something else"_s, u"b"_s,
            u"e520 10000 0 0 0 0"_s, u"0"_s, u"660800011000003"_s);
    sys.setInhibited(u"input6"_s, u"1"_s);           // …and whatever took its number is someone else's business
    CHECK(DeviceLock::releaseRecorded(sys.record(), sys.classRoot()) == 1);
    CHECK(sys.inhibited(u"input0"_s) == u"0"_s);
    CHECK(sys.inhibited(u"input6"_s) == u"1"_s);
}

TEST_CASE("A device in the same place is released even if it now gives another name") {
    // Identity is where the device is, not what it calls itself: when in doubt, the owner gets the keyboard back.
    FakeSys sys;
    DeviceLock lock(sys.classRoot(), sys.record());
    REQUIRE(lock.lock(u":1.42"_s));
    sys.rename(u"input0"_s, u"Another keyboard"_s);
    lock.unlock();
    CHECK(sys.inhibited(u"input0"_s) == u"0"_s);
    CHECK(sys.inhibited(u"input6"_s) == u"0"_s);
}

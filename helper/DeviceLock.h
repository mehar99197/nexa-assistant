#pragma once

#include "core/Result.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace nexa {

// Hardcore mode's lock (Memento for the release): the physical keyboards and pointers are switched off through the
// kernel's own input inhibit — /sys/class/input/inputN/inhibited — so no event of theirs reaches anyone, and the keys
// held at that moment are released by the kernel. Which devices: inputdevices::classify.
//
// What was switched off is written to a record (0600, in a 0700 directory) before anything else happens, so a helper
// that dies with the laptop locked can be undone from the record alone: the unit's ExecStopPost runs
// `nexa-helper --release-input`, which is releaseRecorded(). A device is recognised by its sysfs name and where it
// really is (its canonical path); one somebody else had already switched off is neither recorded nor released.
//
// It reads device metadata only — names, capability bitmaps, `inhibited` — never an event.
class DeviceLock {
public:
    explicit DeviceLock(QString classRoot = defaultClassRoot(), QString recordPath = defaultRecordPath());

    static QString defaultClassRoot();      // /sys/class/input
    static QString defaultRecordPath();     // /run/nexa-helper/locked-input.json

    // Switches off every keyboard and pointer not yet held, for `owner` (the caller's bus name), and returns the names
    // of all it holds. Again while locked it rescans: a device plugged in since is locked too. Another owner while
    // locked: PermissionDenied. Nothing to lock: NotFound. A device that cannot be switched off undoes the whole lock.
    Result<QStringList> lock(const QString &owner);
    // Gives every held device back and deletes the record.
    void unlock();
    bool isLocked() const { return !held_.isEmpty(); }
    QString owner() const { return owner_; }

    // Releases what a record names and deletes it; the number of devices given back.
    static int releaseRecorded(const QString &recordPath = defaultRecordPath(),
                               const QString &classRoot = defaultClassRoot());

private:
    struct Device {
        QString sysfs;      // "input0"
        QString name;       // "AT Translated Set 2 keyboard"
        QString path;       // its canonical sysfs path
    };

    static bool setInhibited(const QString &classRoot, const Device &device, bool on, QString *why = nullptr);
    static bool stillThere(const QString &classRoot, const Device &device);
    static QList<Device> load(const QString &recordPath);
    bool save() const;

    QString classRoot_;
    QString recordPath_;
    QString owner_;
    QList<Device> held_;
};

} // namespace nexa

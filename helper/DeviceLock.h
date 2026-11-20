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
// A device the kernel will not give back (its driver failed to open again) is never forgotten: it stays in the record
// as stuck, and retryStuck() tries again until it is back.
//
// Exempt are the devices uinput makes (under /sys/devices/virtual/input: Nexa's own keyboard and pointer); a Bluetooth
// keyboard or mouse, made through uhid under /sys/devices/virtual/misc, is locked like any other.
//
// It reads device metadata only — names, capability bitmaps, `inhibited` — never an event.
class DeviceLock {
public:
    explicit DeviceLock(QString classRoot = defaultClassRoot(), QString recordPath = defaultRecordPath());

    static QString defaultClassRoot();      // /sys/class/input
    static QString defaultRecordPath();     // /run/nexa-helper/locked-input.json

    // Switches off every keyboard and pointer not yet held, for `owner` (the caller's bus name), and returns the names
    // of all it holds. Again while locked it rescans: a device plugged in since is locked too (one that cannot be is
    // left working, and the lock holds). Another owner while locked: Unauthorized. Nothing to lock: NotFound. On the
    // first lock, a device that cannot be switched off undoes the whole lock.
    Result<QStringList> lock(const QString &owner);
    // Gives every held device back; the record goes once nothing is stuck. False: some are stuck (retryStuck).
    bool unlock();
    bool isLocked() const { return !held_.isEmpty(); }
    QString owner() const { return owner_; }
    bool hasStuck() const { return !stuck_.isEmpty(); }
    int retryStuck();               // the number still stuck

    // Releases what a record names, trying a few times; what still will not come back stays in the record. The number
    // of devices given back.
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
    static bool write(const QString &recordPath, const QString &owner, const QList<Device> &devices);
    bool save() const;              // held and stuck; no record when there is neither

    QString classRoot_;
    QString recordPath_;
    QString owner_;
    QList<Device> held_;
    QList<Device> stuck_;           // switched off by a lock, and not given back by the kernel yet
};

} // namespace nexa

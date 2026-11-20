#pragma once

#include "core/Result.h"

#include <QObject>
#include <QStringList>

#include <functional>

namespace nexa {

// The lock on the laptop's own keyboard and pointer that hardcore mode holds (Strategy): system/input/HelperInputLock
// through the root helper, a fake in tests. Holding it also keeps the screen awake, so the owner can watch.
class IInputLock : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // Whether a lock can be had at all; `why` says what is missing (the helper, or a helper new enough).
    virtual bool canLock(QString *why = nullptr) const = 0;
    // Locks; `done` gets the names of the devices held, or why not. Kept up (the helper's heartbeat) until unlock().
    virtual void lock(std::function<void(Result<QStringList>)> done) = 0;
    virtual void unlock() = 0;

signals:
    // The lock ended without unlock(): the helper gave the devices back by itself, or went away.
    void lost(const QString &reason);
};

} // namespace nexa

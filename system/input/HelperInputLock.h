#pragma once

#include "core/mission/IInputLock.h"

#include <QTimer>

namespace nexa {

class HelperClient;
class ScreenAwake;

// Hardcore mode's lock through the root helper (Adapter: IInputLock over HelperClient's LockInput / UnlockInput).
// While held it calls LockInput every kHeartbeatMs — the helper gives the devices back by itself when that stops — and
// keeps the screen awake. The helper ending the lock (InputUnlocked), going away, or not answering a heartbeat is
// `lost`. Going away itself, it unlocks before it goes.
class HelperInputLock : public IInputLock {
    Q_OBJECT
public:
    static constexpr int kHeartbeatMs = 5'000;

    HelperInputLock(HelperClient *helper, ScreenAwake *awake, QObject *parent = nullptr);
    ~HelperInputLock() override;

    bool canLock(QString *why = nullptr) const override;
    void lock(std::function<void(Result<QStringList>)> done) override;
    void unlock() override;

private:
    void beat();
    // `tellHelper`: the helper may still hold the devices (a heartbeat went unanswered), so it is told to let go.
    void lose(const QString &reason, bool tellHelper);

    HelperClient *helper_;
    ScreenAwake *awake_;
    bool held_{false};
    QTimer heartbeat_;
};

} // namespace nexa

#pragma once

#include "core/mission/IInputLock.h"
#include "core/mission/Task.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace nexa {

class MissionControl;

// Hardcore mode (State): while a hardcore task is active — queued, running, waiting for the owner's answer or for its
// sub-task, not paused and not finished — the laptop's own keyboard and mouse are locked through an IInputLock, so
// nobody at the laptop can spoil what the task types and clicks; the owner watches. A countdown comes first, so the
// hand leaves the keys and a fast-path task, done in a moment, never locks. The lock ends when no hardcore task is
// active, when the owner says unlock (unlockNow: the voice, the phone, "stop everything"), or when it is lost (Nexa
// stopped answering the helper, the helper went away) — and then no task is hardcore any more and the switch is off,
// so it does not lock again behind the owner's back. There is no time limit: that was the owner's choice.
//
// The switch marks the tasks started while it is on (AppController); this class only watches the board.
class HardcoreMode : public QObject {
    Q_OBJECT
public:
    enum class State {
        Off,        // the switch off, nothing to lock
        Armed,      // the switch on, no hardcore task yet
        Counting,   // a hardcore task is active: the lock comes in a moment
        Locking,    // the helper was asked (its password prompt may be up)
        Locked,
        Failed,     // the lock was refused: the tasks run unlocked, reason() says why
    };
    static constexpr int kCountdownMs = 3'000;

    HardcoreMode(MissionControl *mission, IInputLock *lock, QObject *parent = nullptr);
    ~HardcoreMode() override;       // a lock still held is given back

    void setSwitch(bool on);        // off while something is held or counted down: as unlockNow
    bool isSwitchOn() const { return switch_; }
    State state() const { return state_; }
    bool holdsInput() const { return state_ == State::Locking || state_ == State::Locked; }
    int secondsLeft() const;        // of the countdown
    QString reason() const { return reason_; }      // why it failed, or why it last ended early
    QStringList devices() const { return devices_; }
    // The line the window shows (empty while off): the countdown, what the lock waits for, or why it could not lock.
    QString banner() const;
    // The keyboard and mouse back now: no task is hardcore any more, the switch is off, the tasks go on.
    void unlockNow(const QString &why);
    bool canLock(QString *why = nullptr) const { return lock_->canLock(why); }
    void setCountdownMs(int ms) { countdownMs_ = ms; }

    static bool wantsLock(const QList<Task> &tasks);

signals:
    void stateChanged(nexa::HardcoreMode::State state);
    void switchChanged(bool on);
    void countdownTick(int secondsLeft);
    void unlocked(const QString &why);      // it ended early: unlockNow, or the lock was lost

private:
    void refresh();                         // the board changed
    void startCountdown();
    void beginLock();
    void onLocked(Result<QStringList> held);
    void settle();                          // Armed or Off, nothing held
    void setState(State state);
    QString waitingFor() const;             // "“tidy the downloads folder”", or "3 tasks"

    MissionControl *mission_;
    IInputLock *lock_;
    bool switch_{false};
    State state_{State::Off};
    QString reason_;
    QStringList devices_;
    int countdownMs_{kCountdownMs};
    QTimer countdown_;                      // the lock, at the end of it
    QTimer ticker_;                         // the banner's seconds
    QElapsedTimer counted_;
    bool unlocking_{false};
};

} // namespace nexa

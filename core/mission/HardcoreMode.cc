#include "core/mission/HardcoreMode.h"

#include "core/Logging.h"
#include "core/mission/MissionControl.h"

#include <QScopedValueRollback>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

HardcoreMode::HardcoreMode(MissionControl *mission, IInputLock *lock, QObject *parent)
    : QObject(parent), mission_(mission), lock_(lock) {
    countdown_.setSingleShot(true);
    connect(&countdown_, &QTimer::timeout, this, &HardcoreMode::beginLock);
    ticker_.setInterval(1'000);
    connect(&ticker_, &QTimer::timeout, this, [this] { emit countdownTick(secondsLeft()); });
    connect(mission_, &MissionControl::taskAdded, this, &HardcoreMode::refresh);
    connect(mission_, &MissionControl::taskChanged, this, &HardcoreMode::refresh);
    connect(mission_, &MissionControl::taskRemoved, this, &HardcoreMode::refresh);
    connect(lock_, &IInputLock::lost, this, [this](const QString &reason) {
        if (state_ == State::Locking || state_ == State::Locked) unlockNow(reason);
    });
}

HardcoreMode::~HardcoreMode() {
    if (holdsInput()) lock_->unlock();
}

bool HardcoreMode::wantsLock(const QList<Task> &tasks) {
    return std::any_of(tasks.cbegin(), tasks.cend(), [](const Task &task) {
        return task.hardcore && !isFinished(task.state) && task.state != TaskState::Paused;
    });
}

void HardcoreMode::setSwitch(bool on) {
    if (on) {
        if (switch_) return;
        switch_ = true;
        emit switchChanged(true);
        if (state_ == State::Off) setState(State::Armed);
        return;
    }
    if (state_ == State::Counting || holdsInput() || state_ == State::Failed) {
        unlockNow(tr("Hardcore mode was switched off."));
        return;
    }
    if (!switch_) return;
    switch_ = false;
    {
        // Off means off: a paused hardcore task resumed later, or asked again, must not lock behind the owner's back.
        const QScopedValueRollback<bool> quiet(unlocking_, true);
        mission_->releaseHardcore();
    }
    emit switchChanged(false);
    setState(State::Off);
}

int HardcoreMode::secondsLeft() const {
    if (state_ != State::Counting) return 0;
    const qint64 left = countdownMs_ - counted_.elapsed();
    return int(std::max<qint64>(1, (left + 999) / 1000));
}

void HardcoreMode::refresh() {
    if (unlocking_) return;
    const bool want = wantsLock(mission_->tasks());
    switch (state_) {
    case State::Off:
    case State::Armed:
        if (want) startCountdown();
        return;
    case State::Counting:
        if (!want) {
            countdown_.stop();
            ticker_.stop();
            settle();
        }
        return;
    case State::Locking:
        return;     // onLocked looks again when the helper answers
    case State::Locked:
        if (!want) {
            lock_->unlock();
            devices_.clear();
            qCInfo(lcApp) << "hardcore: no hardcore task is active, the keyboard and mouse are back";
            settle();
        } else {
            emit stateChanged(state_);      // what it waits for may have changed (the banner)
        }
        return;
    case State::Failed:
        if (!want) settle();
        return;
    }
}

void HardcoreMode::startCountdown() {
    counted_.start();
    countdown_.start(countdownMs_);
    ticker_.start();
    setState(State::Counting);
    emit countdownTick(secondsLeft());
}

void HardcoreMode::beginLock() {
    ticker_.stop();
    if (!wantsLock(mission_->tasks())) {
        settle();
        return;
    }
    if (QString why; !lock_->canLock(&why)) {
        reason_ = why;
        qCWarning(lcApp).noquote() << "hardcore: cannot lock:" << why;
        setState(State::Failed);
        return;
    }
    setState(State::Locking);
    const QPointer<HardcoreMode> self(this);
    lock_->lock([self](Result<QStringList> held) {
        if (self) self->onLocked(std::move(held));
    });
}

void HardcoreMode::onLocked(Result<QStringList> held) {
    if (state_ != State::Locking) {
        // Given up while the helper was being asked (unlockNow): whatever it locked goes back.
        if (held) lock_->unlock();
        return;
    }
    if (!held) {
        reason_ = held.error().message;
        qCWarning(lcApp).noquote() << "hardcore: the keyboard and mouse could not be locked:" << reason_;
        setState(wantsLock(mission_->tasks()) ? State::Failed : (switch_ ? State::Armed : State::Off));
        return;
    }
    if (!wantsLock(mission_->tasks())) {        // the task ended while the helper was being asked
        lock_->unlock();
        settle();
        return;
    }
    devices_ = held.value();
    reason_.clear();
    qCInfo(lcApp).noquote() << "hardcore: locked" << devices_.join(u", "_s);
    setState(State::Locked);
}

void HardcoreMode::unlockNow(const QString &why) {
    const bool held = state_ == State::Counting || holdsInput() || state_ == State::Failed;
    const QScopedValueRollback<bool> quiet(unlocking_, true);
    countdown_.stop();
    ticker_.stop();
    if (state_ == State::Locked) lock_->unlock();       // Locking: onLocked gives it back when the answer comes
    devices_.clear();
    reason_ = why;
    mission_->releaseHardcore();
    if (switch_) {
        switch_ = false;
        emit switchChanged(false);
    }
    if (held) {
        qCInfo(lcApp).noquote() << "hardcore: unlocked early:" << why;
        emit unlocked(why);     // before the state: whoever shows it tells why, not just that it ended
    }
    setState(State::Off);
}

void HardcoreMode::settle() {
    setState(switch_ ? State::Armed : State::Off);
}

void HardcoreMode::setState(State state) {
    if (state_ == state) return;
    static const char *const names[] = {"off", "armed", "counting", "locking", "locked", "failed"};
    qCDebug(lcApp) << "hardcore:" << names[int(state_)] << "->" << names[int(state)];
    state_ = state;
    emit stateChanged(state_);
}

QString HardcoreMode::waitingFor() const {
    QStringList titles;
    for (const Task &task : mission_->tasks()) {
        if (task.depth == 0 && task.hardcore && !isFinished(task.state) && task.state != TaskState::Paused) {
            titles.append(task.title);
        }
    }
    if (titles.size() == 1) return tr("“%1” is done").arg(titles.first());
    return tr("%n tasks are done", nullptr, int(titles.size()));
}

QString HardcoreMode::banner() const {
    switch (state_) {
    case State::Off: return {};
    case State::Armed: return tr("Hardcore mode is on: the next task locks your keyboard and mouse until it is done.");
    case State::Counting:
        return tr("Hardcore mode: your keyboard and mouse lock in %1 s, until %2.").arg(secondsLeft()).arg(waitingFor());
    case State::Locking: return tr("Hardcore mode: locking your keyboard and mouse…");
    case State::Locked: return tr("Keyboard and mouse locked until %1.").arg(waitingFor());
    case State::Failed: return tr("Hardcore mode could not lock the keyboard and mouse: %1").arg(reason_);
    }
    return {};
}

} // namespace nexa

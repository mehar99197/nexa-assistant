#include "system/input/HelperInputLock.h"

#include "core/Logging.h"
#include "system/helper/HelperClient.h"
#include "system/screen/ScreenAwake.h"

#include <QPointer>

using namespace Qt::StringLiterals;

namespace nexa {

HelperInputLock::HelperInputLock(HelperClient *helper, ScreenAwake *awake, QObject *parent)
    : IInputLock(parent), helper_(helper), awake_(awake) {
    heartbeat_.setInterval(kHeartbeatMs);
    connect(&heartbeat_, &QTimer::timeout, this, &HelperInputLock::beat);
    connect(helper_, &HelperClient::inputUnlocked, this, [this](const QString &reason) {
        if (held_) lose(reason);
    });
    connect(helper_, &HelperClient::helperGone, this, [this] {
        // Its ExecStopPost has given the devices back.
        if (held_) lose(tr("The root helper stopped."));
    });
}

HelperInputLock::~HelperInputLock() {
    if (held_ || unlocking_) helper_->unlockInputAndWait();
}

bool HelperInputLock::canLock(QString *why) const {
    if (HelperClient::isInstalled()) return true;
    if (why) {
        *why = tr("Hardcore mode needs the root helper. Install it with: %1").arg(HelperClient::installCommand());
    }
    return false;
}

void HelperInputLock::lock(std::function<void(Result<QStringList>)> done) {
    const QPointer<HelperInputLock> self(this);
    helper_->lockInput([self, done = std::move(done)](Result<QStringList> held) {
        if (self && held) {
            self->held_ = true;
            self->heartbeat_.start();
            self->awake_->hold(tr("A hardcore task is running: the keyboard and mouse are locked"));
        }
        done(std::move(held));
    });
}

void HelperInputLock::unlock() {
    heartbeat_.stop();
    awake_->release();
    if (!held_) return;
    held_ = false;
    unlocking_ = true;
    const QPointer<HelperInputLock> self(this);
    helper_->unlockInput([self](Result<void> done) {
        if (self) self->unlocking_ = false;
        if (!done) qCWarning(lcApp).noquote() << "hardcore: the helper did not take the unlock:" << done.error().message;
    });
}

void HelperInputLock::beat() {
    if (!held_) return;
    const QPointer<HelperInputLock> self(this);
    helper_->lockInput([self](Result<QStringList> held) {
        if (!self) return;
        if (!self->held_) {
            // A heartbeat answered after the lock was let go (a helper restarted, a reply that came late): the helper
            // now holds a lock nobody wants.
            if (held) self->helper_->unlockInput([](Result<void>) {});
            return;
        }
        if (!held) self->lose(tr("The root helper stopped answering: %1").arg(held.error().message));
    });
}

void HelperInputLock::lose(const QString &reason) {
    if (!held_) return;
    held_ = false;
    heartbeat_.stop();
    awake_->release();
    helper_->unlockInput([](Result<void>) {});
    emit lost(reason);
}

} // namespace nexa

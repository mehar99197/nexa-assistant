#include "system/android/PhoneMirror.h"

#include "core/Logging.h"
#include "system/android/AndroidDevice.h"

#include <QElapsedTimer>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRetryMs = 3'000;     // after a failure: the phone is busy, or going

} // namespace

PhoneMirror::PhoneMirror(AndroidDevice *device, QObject *parent) : QObject(parent), device_(device) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &PhoneMirror::request);
}

void PhoneMirror::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    emit activeChanged(active);
    if (active) {
        request();
    } else {
        timer_.stop();
        ++generation_;      // whatever is on its way now is not wanted
        inFlight_ = false;
    }
}

void PhoneMirror::refresh() {
    if (!active_ || inFlight_) return;
    timer_.stop();
    request();
}

void PhoneMirror::request() {
    if (!active_ || inFlight_) return;
    inFlight_ = true;
    const quint64 generation = generation_;
    QElapsedTimer timer;
    timer.start();
    device_->screenshot([this, generation, timer](Result<AndroidDevice::Shot> shot) {
        if (generation != generation_) return;
        inFlight_ = false;
        if (!shot) {
            qCDebug(lcApp) << "phone mirror:" << shot.error().message;
            emit failed(shot.error().message);
            schedule(kRetryMs);
            return;
        }
        lastFrame_ = shot.value().image;
        screenSize_ = shot.value().realSize;
        emit frame(lastFrame_, timer.elapsed());
        schedule(intervalMs_);
    });
}

void PhoneMirror::schedule(int delayMs) {
    if (!active_) return;
    timer_.start(delayMs);
}

} // namespace nexa

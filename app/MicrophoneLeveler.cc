#include "app/MicrophoneLeveler.h"

#include "system/audio/WpctlAudioController.h"
#include "voice/pipeline/VoicePipeline.h"

#include <QTimer>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kSettleMs = 700;      // a change of the input's amplifier jolts the signal for a moment
constexpr int kMeasureMs = 2'000;

bool usable(const MicrophoneCheck::Stats &stats) {
    return !MicrophoneCheck::nextVolume(100, stats);    // what "Lower it" would stop at
}

} // namespace

MicrophoneLeveler::MicrophoneLeveler(WpctlAudioController *microphone, VoicePipeline *voice, QObject *parent)
    : QObject(parent), microphone_(microphone), voice_(voice) {}

void MicrophoneLeveler::run(std::function<void(QString summary)> done) {
    if (running_ || !microphone_ || !voice_) return;
    running_ = true;
    done_ = std::move(done);
    microphone_->volume([this](Result<VolumeState> state) {
        if (!state) {
            finish(tr("Could not read the microphone's volume: %1").arg(state.error().message));
            return;
        }
        startPercent_ = percent_ = state.value().percent;
        measure();
    });
}

void MicrophoneLeveler::measure() {
    QTimer::singleShot(kSettleMs, this, [this] {
        voice_->measureLevels(kMeasureMs, [this](MicrophoneCheck::Stats stats) {
            const std::optional<int> next = MicrophoneCheck::nextVolume(percent_, stats);
            if (!next) {
                if (usable(stats) && percent_ == startPercent_) {
                    finish(tr("The microphone is fine at %1 %; nothing to lower.").arg(percent_));
                } else if (usable(stats)) {
                    finish(tr("Microphone input lowered from %1 % to %2 %; nothing clips now.").arg(startPercent_).arg(percent_));
                } else {
                    finish(tr("Microphone input lowered from %1 % to %2 %; it still clips or roars — move the laptop "
                              "out of the fan's wind.").arg(startPercent_).arg(percent_));
                }
                return;
            }
            microphone_->setVolume(*next, [this](Result<VolumeState> state) {
                if (!state) {
                    finish(tr("Could not change the microphone's volume: %1").arg(state.error().message));
                    return;
                }
                percent_ = state.value().percent;
                measure();
            });
        });
    });
}

void MicrophoneLeveler::finish(const QString &summary) {
    running_ = false;
    if (done_) std::exchange(done_, nullptr)(summary);
}

} // namespace nexa

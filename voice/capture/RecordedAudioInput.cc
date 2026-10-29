#include "voice/capture/RecordedAudioInput.h"

namespace nexa {

namespace {

constexpr int kTickMs = 100;
constexpr qsizetype kTickBytes = 16'000 * 2 / 10;   // 100 ms of 16 kHz mono Int16
constexpr int kSilenceTicks = 30;                    // 3 s after a recording

} // namespace

RecordedAudioInput::RecordedAudioInput(QObject *parent) : IAudioInput(parent) {
    timer_.setInterval(kTickMs);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &RecordedAudioInput::tick);
}

Result<void> RecordedAudioInput::start() {
    active_ = true;
    timer_.start();
    return {};
}

void RecordedAudioInput::stop() {
    active_ = false;
    timer_.stop();
}

void RecordedAudioInput::play(const QByteArray &pcm16k) {
    queue_.append(pcm16k);
    silenceLeft_ = kSilenceTicks;
}

void RecordedAudioInput::tick() {
    if (!queue_.isEmpty()) {
        const QByteArray chunk = queue_.left(kTickBytes);
        queue_.remove(0, chunk.size());
        emit audio(chunk);
        return;
    }
    emit audio(QByteArray(kTickBytes, '\0'));
    if (silenceLeft_ > 0 && --silenceLeft_ == 0) emit drained();
}

} // namespace nexa

#include "voice/listen/AudioRing.h"

#include "voice/capture/EnergyVad.h"

#include <algorithm>

namespace nexa {

namespace {

constexpr qsizetype kSampleRate = 16'000;

} // namespace

AudioRing::AudioRing(int seconds) : capacityBytes_(qsizetype(std::max(1, seconds)) * kSampleRate * 2) {}

void AudioRing::append(QByteArrayView pcm16k) {
    buffer_.append(pcm16k.data(), pcm16k.size() / 2 * 2);
    end_ += pcm16k.size() / 2;
    if (buffer_.size() > capacityBytes_) buffer_.remove(0, buffer_.size() - capacityBytes_);
}

void AudioRing::reset() {
    buffer_.clear();
    end_ = 0;
}

QByteArray AudioRing::slice(qint64 from, qint64 to) const {
    from = std::max(from, begin());
    to = std::min(to, end_);
    if (to <= from) return {};
    return buffer_.mid(qsizetype(from - begin()) * 2, qsizetype(to - from) * 2);
}

double AudioRing::levelDb(qint64 from, qint64 to) const {
    const QByteArray audio = slice(from, to);
    return EnergyVad::rmsDb(reinterpret_cast<const qint16 *>(audio.constData()), audio.size() / 2);
}

} // namespace nexa

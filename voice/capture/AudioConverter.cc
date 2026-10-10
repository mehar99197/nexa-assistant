#include "voice/capture/AudioConverter.h"

#include <algorithm>
#include <cmath>

namespace nexa {

namespace {

void appendSample(QByteArray &out, float value) {
    const auto sample = qint16(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    out.append(reinterpret_cast<const char *>(&sample), sizeof sample);
}

} // namespace

AudioConverter::AudioConverter(const QAudioFormat &input, int outputRate)
    : input_(input),
      outputRate_(outputRate),
      passthrough_(input.sampleFormat() == QAudioFormat::Int16 && input.channelCount() == 1
                   && input.sampleRate() == outputRate),
      step_(input.sampleRate() > 0 ? double(input.sampleRate()) / outputRate : 1.0) {}

void AudioConverter::reset() {
    partial_.clear();
    phase_ = 0.0;
    sum_ = 0.0;
    count_ = 0;
    previous_ = 0.0f;
    position_ = 0.0;
    havePrevious_ = false;
}

QByteArray AudioConverter::convert(QByteArrayView data) {
    const int frameBytes = input_.bytesPerFrame();
    if (frameBytes <= 0) return {};

    QByteArray input = partial_ + data.toByteArray();
    const qsizetype usable = input.size() - input.size() % frameBytes;
    partial_ = input.sliced(usable);
    input.truncate(usable);
    if (passthrough_) return input;

    const int channels = input_.channelCount();
    const int sampleBytes = input_.bytesPerSample();
    QByteArray out;
    out.reserve(qsizetype(double(usable / frameBytes) / step_ + 2) * 2);
    for (qsizetype frame = 0; frame < usable; frame += frameBytes) {
        float mono = 0.0f;
        for (int channel = 0; channel < channels; ++channel) {
            mono += input_.normalizedSampleValue(input.constData() + frame + channel * sampleBytes);
        }
        resample(mono / float(channels), out);
    }
    return out;
}

void AudioConverter::resample(float sample, QByteArray &out) {
    if (step_ == 1.0) {
        appendSample(out, sample);
        return;
    }
    if (step_ > 1.0) {
        sum_ += sample;
        ++count_;
        phase_ += 1.0;
        if (phase_ >= step_) {
            appendSample(out, float(sum_ / count_));
            phase_ -= step_;
            sum_ = 0.0;
            count_ = 0;
        }
        return;
    }
    // Upsampling: outputs at times 0, step, 2·step … measured in input samples.
    if (!havePrevious_) {
        previous_ = sample;
        havePrevious_ = true;
        return;
    }
    while (position_ < 1.0) {
        appendSample(out, previous_ + (sample - previous_) * float(position_));
        position_ += step_;
    }
    position_ -= 1.0;
    previous_ = sample;
}

} // namespace nexa

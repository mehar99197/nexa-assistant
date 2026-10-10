#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QByteArrayView>

namespace nexa {

// Converts captured audio in any QAudioFormat to mono Int16 at 16 kHz (what the VAD and the
// recognisers expect): channels are averaged, then the rate is converted — box-filter decimation
// when downsampling (a cheap anti-alias filter), linear interpolation when upsampling. State is
// kept between calls, so a stream fed in arbitrary pieces converts exactly like one fed at once.
class AudioConverter {
public:
    static constexpr int kOutputRate = 16'000;

    explicit AudioConverter(const QAudioFormat &input, int outputRate = kOutputRate);

    QByteArray convert(QByteArrayView data);
    void reset();

    bool isPassthrough() const { return passthrough_; }

private:
    void resample(float sample, QByteArray &out);

    QAudioFormat input_;
    int outputRate_;
    bool passthrough_{false};
    double step_{1.0};      // input samples per output sample
    QByteArray partial_;    // bytes of an incomplete input frame

    // Downsampling: average the input samples that fall into each output period.
    double phase_{0.0};
    double sum_{0.0};
    int count_{0};

    // Upsampling: interpolate between the previous and the current input sample.
    float previous_{0.0f};
    double position_{0.0};
    bool havePrevious_{false};
};

} // namespace nexa

#pragma once

#include <QByteArray>
#include <QByteArrayView>

namespace nexa {

// The last seconds of raw microphone audio (16 kHz mono Int16), addressed by sample position since reset().
// The detectors may hear the audio cleaned; a request or an utterance is cut from here, as it was said. Pure.
class AudioRing {
public:
    // 16 s: a request after the name may run 15 s, and is cut from here with what came just before it.
    explicit AudioRing(int seconds = 16);

    void append(QByteArrayView pcm16k);
    void reset();       // position 0 again

    qint64 begin() const { return end_ - buffer_.size() / 2; }   // the oldest sample still held
    qint64 end() const { return end_; }                         // samples appended since reset()

    // The audio of [from, to), clamped to what is still held.
    QByteArray slice(qint64 from, qint64 to) const;
    // Its RMS level in dBFS (-100 for silence or nothing).
    double levelDb(qint64 from, qint64 to) const;

private:
    qsizetype capacityBytes_;
    QByteArray buffer_;
    qint64 end_{0};
};

} // namespace nexa

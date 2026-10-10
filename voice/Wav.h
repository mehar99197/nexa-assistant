#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>

namespace nexa {

// Interleaved signed 16-bit little-endian PCM plus its format; the unit voice code passes around.
struct PcmAudio {
    QByteArray samples;
    int sampleRate{16'000};
    int channels{1};

    qsizetype frameCount() const { return channels > 0 ? samples.size() / (2 * channels) : 0; }
    qint64 durationMs() const { return sampleRate > 0 ? frameCount() * 1000 / sampleRate : 0; }
    bool sameFormat(const PcmAudio &other) const {
        return sampleRate == other.sampleRate && channels == other.channels;
    }
};

// RIFF/WAVE encoding and decoding (pure). decode() accepts integer PCM (8/16/24/32-bit) and float
// (32/64-bit), plain or WAVE_FORMAT_EXTENSIBLE, and converts to 16-bit. A data chunk whose size is
// unknown (0 or 0xFFFFFFFF, as streaming TTS servers send it) extends to the end of the file.
namespace wav {

QByteArray encode(const PcmAudio &audio);
Result<PcmAudio> decode(QByteArrayView file);

} // namespace wav

} // namespace nexa

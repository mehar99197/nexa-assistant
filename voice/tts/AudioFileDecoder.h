#pragma once

#include "core/Result.h"
#include "voice/Wav.h"

#include <QByteArray>
#include <QObject>

#include <functional>

namespace nexa {

// Encoded audio (MP3, Ogg, AAC… as cloud voices return it) → 16-bit PCM. WAV is parsed directly;
// everything else goes through QAudioDecoder (FFmpeg backend), off the critical path of playback.
class AudioFileDecoder : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<PcmAudio>)>;

    explicit AudioFileDecoder(QObject *parent = nullptr);

    // `done` runs exactly once, unless this object is destroyed first.
    void decode(const QByteArray &encoded, Done done);
};

} // namespace nexa

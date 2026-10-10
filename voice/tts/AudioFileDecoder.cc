#include "voice/tts/AudioFileDecoder.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QBuffer>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace Qt::StringLiterals;

namespace nexa {

AudioFileDecoder::AudioFileDecoder(QObject *parent) : QObject(parent) {}

void AudioFileDecoder::decode(const QByteArray &encoded, Done done) {
    if (encoded.startsWith("RIFF")) {
        done(wav::decode(encoded));
        return;
    }

    // One decoder per clip; it and its input buffer are released when decoding ends.
    auto *decoder = new QAudioDecoder(this);
    auto *input = new QBuffer(decoder);
    input->setData(encoded);
    input->open(QIODevice::ReadOnly);
    decoder->setSourceDevice(input);    // native format (MP3 decodes to float); converted below

    auto audio = std::make_shared<PcmAudio>();
    auto callback = std::make_shared<Done>(std::move(done));
    const auto finish = [decoder, callback](Result<PcmAudio> result) {
        decoder->disconnect();
        decoder->deleteLater();
        if (*callback) std::exchange(*callback, nullptr)(std::move(result));
    };

    connect(decoder, &QAudioDecoder::bufferReady, this, [decoder, audio] {
        const QAudioBuffer buffer = decoder->read();
        const QAudioFormat format = buffer.format();
        const int sampleBytes = format.bytesPerSample();
        if (sampleBytes <= 0) return;
        audio->sampleRate = format.sampleRate();
        audio->channels = format.channelCount();
        const char *data = buffer.constData<char>();
        const qsizetype samples = buffer.byteCount() / sampleBytes;
        const qsizetype offset = audio->samples.size();
        audio->samples.resize(offset + samples * 2);
        auto *out = reinterpret_cast<qint16 *>(audio->samples.data() + offset);
        for (qsizetype i = 0; i < samples; ++i) {
            const float value = std::clamp(format.normalizedSampleValue(data + i * sampleBytes), -1.0f, 1.0f);
            out[i] = qint16(std::lround(value * 32767.0f));
        }
    });
    connect(decoder, &QAudioDecoder::finished, this, [audio, finish] {
        if (audio->samples.isEmpty()) {
            finish(Error{ErrorCode::Parse, u"The audio clip contained no sound."_s});
        } else {
            finish(std::move(*audio));
        }
    });
    connect(decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
            [decoder, finish](QAudioDecoder::Error) {
                finish(Error{ErrorCode::Parse, u"Cannot decode the audio clip: %1"_s.arg(decoder->errorString())});
            });
    decoder->start();
}

} // namespace nexa

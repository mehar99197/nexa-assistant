#include "voice/Wav.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

namespace nexa::wav {

namespace {

constexpr quint16 kFormatPcm = 1;
constexpr quint16 kFormatFloat = 3;
constexpr quint16 kFormatExtensible = 0xFFFE;

quint16 u16(const char *data) { return qFromLittleEndian<quint16>(data); }
quint32 u32(const char *data) { return qFromLittleEndian<quint32>(data); }

void putU16(QByteArray &out, quint16 value) {
    char bytes[2];
    qToLittleEndian(value, bytes);
    out.append(bytes, 2);
}

void putU32(QByteArray &out, quint32 value) {
    char bytes[4];
    qToLittleEndian(value, bytes);
    out.append(bytes, 4);
}

qint16 fromFloat(double value) {
    return qint16(std::lround(std::clamp(value, -1.0, 1.0) * 32767.0));
}

Error parseError(const QString &message) {
    return Error{ErrorCode::Parse, u"Not a usable WAV file: %1"_s.arg(message)};
}

// One sample of the given encoding → 16-bit.
qint16 convertSample(const char *sample, quint16 format, int bits) {
    if (format == kFormatFloat) {
        if (bits == 32) {
            float value;
            std::memcpy(&value, sample, 4);
            return fromFloat(qFromLittleEndian(value));
        }
        double value;
        std::memcpy(&value, sample, 8);
        return fromFloat(qFromLittleEndian(value));
    }
    switch (bits) {
    case 8: {
        // 8-bit WAV samples are unsigned, with silence at 128.
        const int value = static_cast<unsigned char>(sample[0]);
        return qint16((value - 128) * 256);
    }
    case 16:
        return qFromLittleEndian<qint16>(sample);
    default:  // 24 and 32: keep the two most significant bytes
        return qFromLittleEndian<qint16>(sample + bits / 8 - 2);
    }
}

} // namespace

QByteArray encode(const PcmAudio &audio) {
    const quint32 dataSize = quint32(audio.samples.size());
    const quint16 blockAlign = quint16(audio.channels * 2);
    QByteArray out;
    out.reserve(44 + audio.samples.size());
    out.append("RIFF");
    putU32(out, 36 + dataSize);
    out.append("WAVEfmt ");
    putU32(out, 16);
    putU16(out, kFormatPcm);
    putU16(out, quint16(audio.channels));
    putU32(out, quint32(audio.sampleRate));
    putU32(out, quint32(audio.sampleRate) * blockAlign);
    putU16(out, blockAlign);
    putU16(out, 16);
    out.append("data");
    putU32(out, dataSize);
    out.append(audio.samples);
    return out;
}

Result<PcmAudio> decode(QByteArrayView file) {
    if (file.size() < 12 || file.first(4) != "RIFF" || file.sliced(8, 4) != "WAVE") {
        return parseError(u"missing RIFF/WAVE header"_s);
    }

    quint16 format = 0;
    int channels = 0;
    int sampleRate = 0;
    int bits = 0;
    bool haveFormat = false;
    qsizetype offset = 12;
    while (offset + 8 <= file.size()) {
        const QByteArrayView id = file.sliced(offset, 4);
        const quint32 declared = u32(file.data() + offset + 4);
        const qsizetype body = offset + 8;
        const qsizetype available = file.size() - body;

        if (id == "fmt ") {
            if (declared < 16 || available < 16) return parseError(u"short fmt chunk"_s);
            const char *fmt = file.data() + body;
            format = u16(fmt);
            channels = u16(fmt + 2);
            sampleRate = int(u32(fmt + 4));
            bits = u16(fmt + 14);
            if (format == kFormatExtensible) {
                if (declared < 40 || available < 40) return parseError(u"short extensible fmt chunk"_s);
                format = u16(fmt + 24);  // first two bytes of the sub-format GUID
            }
            haveFormat = true;
        } else if (id == "data") {
            if (!haveFormat) return parseError(u"data before fmt"_s);
            if (format != kFormatPcm && format != kFormatFloat) {
                return parseError(u"compressed audio (format %1)"_s.arg(format));
            }
            const bool bitsOk = format == kFormatFloat ? (bits == 32 || bits == 64)
                                                       : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
            if (!bitsOk) return parseError(u"%1-bit samples"_s.arg(bits));
            if (channels < 1 || channels > 8 || sampleRate < 1000 || sampleRate > 384'000) {
                return parseError(u"%1 channels at %2 Hz"_s.arg(channels).arg(sampleRate));
            }

            const bool unknownSize = declared == 0 || declared == 0xFFFFFFFFu;
            const qsizetype size = unknownSize ? available : std::min<qsizetype>(declared, available);
            const int bytesPerSample = bits / 8;
            const qsizetype samples = size / (qsizetype(bytesPerSample) * channels) * channels;

            PcmAudio audio{.sampleRate = sampleRate, .channels = channels};
            audio.samples.resize(samples * 2);
            auto *out = reinterpret_cast<qint16 *>(audio.samples.data());
            const char *in = file.data() + body;
            for (qsizetype i = 0; i < samples; ++i) {
                out[i] = qToLittleEndian(convertSample(in + i * bytesPerSample, format, bits));
            }
            return audio;
        }

        if (declared > quint32(available)) break;
        offset = body + qsizetype(declared) + (declared & 1);  // chunks are padded to even sizes
    }
    return parseError(haveFormat ? u"no data chunk"_s : u"no fmt chunk"_s);
}

} // namespace nexa::wav

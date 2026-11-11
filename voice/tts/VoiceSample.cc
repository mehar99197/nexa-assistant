#include "voice/tts/VoiceSample.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr QFileDevice::Permissions kPrivate = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr int kFrameMs = 20;
constexpr int kBreathMs = 150;

// The loudness of each 20 ms frame, 0 … 1 of full scale.
std::vector<double> frameLevels(const PcmAudio &audio) {
    const int frame = std::max(1, audio.sampleRate * kFrameMs / 1000) * std::max(1, audio.channels);
    const auto *samples = reinterpret_cast<const qint16 *>(audio.samples.constData());
    const qsizetype count = audio.samples.size() / 2;
    std::vector<double> levels;
    for (qsizetype start = 0; start + frame <= count; start += frame) {
        double sum = 0;
        for (int i = 0; i < frame; ++i) {
            const double value = samples[start + i] / 32768.0;
            sum += value * value;
        }
        levels.push_back(std::sqrt(sum / frame));
    }
    return levels;
}

} // namespace

QString VoiceSample::defaultDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa"_s;
}

QString VoiceSample::wavPath(const QString &dir) {
    return dir + u"/voice-sample.wav"_s;
}

QString VoiceSample::infoPath(const QString &dir) {
    return dir + u"/voice-sample.json"_s;
}

QString VoiceSample::passage() {
    return u"Hello, this is my own voice. From now on, Nexa will speak with it. "
           "The quick brown fox jumps over the lazy dog, and today the weather is bright and clear."_s;
}

PcmAudio VoiceSample::trimmed(const PcmAudio &audio) {
    const std::vector<double> levels = frameLevels(audio);
    if (levels.empty()) return audio;
    // The quiet is what the quietest tenth of the frames sounds like; sound is well above it — above a floor of its
    // own too, so a silent room's hiss never counts as speech, and below a ceiling, so a recording that is all
    // speech is not all "quiet".
    std::vector<double> sorted = levels;
    std::sort(sorted.begin(), sorted.end());
    const double floor = sorted[sorted.size() / 10];
    const double threshold = std::clamp(floor * 4.0, 0.004, 0.02);
    size_t first = 0;
    while (first < levels.size() && levels[first] < threshold) ++first;
    if (first == levels.size()) return PcmAudio{{}, audio.sampleRate, audio.channels};
    size_t last = levels.size() - 1;
    while (last > first && levels[last] < threshold) --last;
    const int breath = kBreathMs / kFrameMs;
    first = first > size_t(breath) ? first - breath : 0;
    last = std::min(levels.size() - 1, last + breath);
    const qsizetype frameBytes = qsizetype(std::max(1, audio.sampleRate * kFrameMs / 1000) * std::max(1, audio.channels)) * 2;
    PcmAudio out{{}, audio.sampleRate, audio.channels};
    out.samples = audio.samples.mid(qsizetype(first) * frameBytes, qsizetype(last - first + 1) * frameBytes);
    const qsizetype maxBytes = qsizetype(audio.sampleRate) * kMaxMs / 1000 * std::max(1, audio.channels) * 2;
    if (out.samples.size() > maxBytes) out.samples.truncate(maxBytes);
    return out;
}

Result<void> VoiceSample::usable(const PcmAudio &audio) {
    const std::vector<double> levels = frameLevels(audio);
    const double loudest = levels.empty() ? 0.0 : *std::max_element(levels.begin(), levels.end());
    if (loudest < 0.01) return Error{ErrorCode::InvalidArgument, u"Nothing was heard — is the microphone muted, or too far away?"_s};
    if (audio.durationMs() < kMinMs) {
        return Error{ErrorCode::InvalidArgument,
                     u"Too short (%1 s of speech): read the whole passage, at your usual pace."_s.arg(audio.durationMs() / 1000.0, 0, 'f', 1)};
    }
    return {};
}

Result<VoiceSample> VoiceSample::load(const QString &dir) {
    QFile wav(wavPath(dir));
    if (!wav.exists()) return Error{ErrorCode::NotFound, u"No voice sample is recorded."_s};
    if (!wav.open(QIODevice::ReadOnly)) return Error{ErrorCode::Io, u"Cannot read the voice sample: %1"_s.arg(wav.errorString())};
    const Result<PcmAudio> audio = wav::decode(wav.readAll());
    if (!audio) return Error{ErrorCode::Parse, u"The voice sample is damaged: %1"_s.arg(audio.error().message)};
    VoiceSample sample;
    sample.audio = audio.value();
    sample.text = passage();
    QFile info(infoPath(dir));
    if (info.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(info.readAll()).object();
        if (const QString text = object.value(u"text"_s).toString(); !text.isEmpty()) sample.text = text;
        sample.recorded = QDateTime::fromString(object.value(u"recorded"_s).toString(), Qt::ISODate);
    }
    if (!sample.recorded.isValid()) sample.recorded = QFileInfo(wav).lastModified();
    return sample;
}

Result<void> VoiceSample::save(const QString &dir) const {
    QDir().mkpath(dir);
    const auto write = [](const QString &path, const QByteArray &bytes) -> Result<void> {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return Error{ErrorCode::Io, u"Cannot save the voice sample: %1"_s.arg(file.errorString())};
        file.write(bytes);
        if (!file.commit()) return Error{ErrorCode::Io, u"Cannot save the voice sample: %1"_s.arg(file.errorString())};
        QFile::setPermissions(path, kPrivate);
        return {};
    };
    if (const Result<void> wrote = write(wavPath(dir), wav::encode(audio)); !wrote) return wrote;
    const QJsonObject info{{u"text"_s, text},
                           {u"recorded"_s, (recorded.isValid() ? recorded : QDateTime::currentDateTime()).toString(Qt::ISODate)},
                           {u"durationMs"_s, qint64(audio.durationMs())}};
    return write(infoPath(dir), QJsonDocument(info).toJson(QJsonDocument::Compact));
}

Result<void> VoiceSample::forget(const QString &dir) {
    for (const QString &path : {wavPath(dir), infoPath(dir)}) {
        if (QFile::exists(path) && !QFile::remove(path)) return Error{ErrorCode::Io, u"Cannot remove the voice sample."_s};
    }
    return {};
}

} // namespace nexa

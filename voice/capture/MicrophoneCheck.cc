#include "voice/capture/MicrophoneCheck.h"

#include "voice/capture/EnergyVad.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace nexa {

namespace {

constexpr qint16 kClipped = 32'700;
constexpr qsizetype kFrame = 320;               // 20 ms at 16 kHz
constexpr qint64 kClipWindowMs = 3'000;
constexpr qint64 kFloorWindowMs = 1'500;
constexpr qint64 kKeptMs = 10'000;
constexpr double kClippedTooMuch = 0.05;
constexpr double kLoudFloorDb = -30.0;
constexpr qint64 kLoudFloorForMs = 10'000;
constexpr qint64 kNoticeEveryMs = 60'000;
constexpr double kVoiceRoomDb = -20.0;
constexpr int kLowestPercent = 5;
constexpr int kStepPercent = 5;

struct Frames {
    std::vector<double> levels;     // of each whole 20 ms frame
    qsizetype clipped{0};
    qsizetype samples{0};
};

Frames framesOf(QByteArrayView pcm16k) {
    Frames frames;
    frames.samples = pcm16k.size() / 2;
    std::vector<qint16> samples(static_cast<size_t>(frames.samples));
    std::memcpy(samples.data(), pcm16k.data(), samples.size() * 2);
    for (const qint16 sample : samples) {
        if (sample >= kClipped || sample <= -kClipped) ++frames.clipped;
    }
    for (qsizetype offset = 0; offset + kFrame <= frames.samples; offset += kFrame) {
        frames.levels.push_back(EnergyVad::rmsDb(samples.data() + offset, kFrame));
    }
    return frames;
}

QString tr(const char *text) {
    return QCoreApplication::translate("nexa::MicrophoneCheck", text);
}

} // namespace

void MicrophoneCheck::feed(QByteArrayView pcm16k, qint64 nowMs) {
    const Frames frames = framesOf(pcm16k);
    const double quietest = frames.levels.empty() ? -100.0 : *std::min_element(frames.levels.begin(), frames.levels.end());
    chunks_.push_back(Chunk{.endMs = nowMs, .samples = frames.samples, .clipped = frames.clipped, .quietestDb = quietest});
    while (!chunks_.empty() && chunks_.front().endMs <= nowMs - kKeptMs) chunks_.pop_front();

    if (floorDb() > kLoudFloorDb) {
        if (loudFloorSinceMs_ < 0) loudFloorSinceMs_ = nowMs - frames.samples / 16;    // from when this audio began
    } else {
        loudFloorSinceMs_ = -1;
    }
}

double MicrophoneCheck::clippedShare() const {
    if (chunks_.empty()) return 0.0;
    const qint64 since = chunks_.back().endMs - kClipWindowMs;
    qsizetype clipped = 0;
    qsizetype samples = 0;
    for (const Chunk &chunk : chunks_) {
        if (chunk.endMs <= since) continue;
        clipped += chunk.clipped;
        samples += chunk.samples;
    }
    return samples > 0 ? double(clipped) / double(samples) : 0.0;
}

double MicrophoneCheck::floorDb() const {
    if (chunks_.empty()) return -100.0;
    const qint64 since = chunks_.back().endMs - kFloorWindowMs;
    double floor = 0.0;
    for (const Chunk &chunk : chunks_) {
        if (chunk.endMs > since) floor = std::min(floor, chunk.quietestDb);
    }
    return floor;
}

bool MicrophoneCheck::overloaded() const {
    if (clippedShare() > kClippedTooMuch) return true;
    return loudFloorSinceMs_ >= 0 && !chunks_.empty() && chunks_.back().endMs - loudFloorSinceMs_ >= kLoudFloorForMs;
}

QString MicrophoneCheck::describe() const {
    if (!overloaded()) return {};
    const QString lower = tr("Lower the input volume in GNOME Settings → Sound → Input.");
    if (const double share = clippedShare(); share > kClippedTooMuch) {
        return tr("The microphone is overloaded (%1 % of the sound is clipped), so Nexa cannot tell when you speak. %2")
            .arg(std::lround(share * 100.0))
            .arg(lower);
    }
    return tr("The microphone hears a loud noise all the time (%1 dBFS), so Nexa cannot tell when you speak. %2")
        .arg(std::lround(floorDb()))
        .arg(lower);
}

std::optional<QString> MicrophoneCheck::notice(qint64 nowMs) {
    if (!overloaded()) return std::nullopt;
    if (lastNoticeMs_ && nowMs - *lastNoticeMs_ < kNoticeEveryMs) return std::nullopt;
    lastNoticeMs_ = nowMs;
    return describe();
}

MicrophoneCheck::Stats MicrophoneCheck::measure(QByteArrayView pcm16k) {
    Frames frames = framesOf(pcm16k);
    Stats stats;
    if (frames.samples > 0) stats.clippedShare = double(frames.clipped) / double(frames.samples);
    if (frames.levels.empty()) return stats;
    std::sort(frames.levels.begin(), frames.levels.end());
    stats.floorDb = frames.levels.front();
    stats.p90Db = frames.levels[std::min(frames.levels.size() - 1, frames.levels.size() * 9 / 10)];
    return stats;
}

std::optional<int> MicrophoneCheck::nextVolume(int percent, const Stats &measured) {
    if (measured.clippedShare <= 0.0 && measured.p90Db <= kVoiceRoomDb) return std::nullopt;
    if (percent <= kLowestPercent) return std::nullopt;
    return std::max(kLowestPercent, percent - kStepPercent);
}

} // namespace nexa

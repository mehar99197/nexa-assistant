#include "voice/capture/EnergyVad.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nexa {

EnergyVad::EnergyVad() : EnergyVad(Settings{}) {}

EnergyVad::EnergyVad(Settings settings)
    : settings_(settings), frameSamples_(qsizetype(settings.sampleRate) * settings.frameMs / 1000) {}

double EnergyVad::rmsDb(const qint16 *samples, qsizetype count) {
    if (count <= 0) return -100.0;
    double energy = 0.0;
    for (qsizetype i = 0; i < count; ++i) energy += double(samples[i]) * samples[i];
    const double rms = std::sqrt(energy / double(count)) / 32768.0;
    return rms > 1e-5 ? 20.0 * std::log10(rms) : -100.0;
}

namespace {

std::vector<double> frameLevels(QByteArrayView pcm, qsizetype frameSamples) {
    const qsizetype frames = frameSamples > 0 ? pcm.size() / 2 / frameSamples : 0;
    std::vector<double> levels;
    levels.reserve(size_t(frames));
    std::vector<qint16> frame(static_cast<size_t>(frameSamples));
    for (qsizetype i = 0; i < frames; ++i) {
        std::copy_n(pcm.data() + i * frameSamples * 2, frameSamples * 2, reinterpret_cast<char *>(frame.data()));
        levels.push_back(EnergyVad::rmsDb(frame.data(), frameSamples));
    }
    return levels;
}

} // namespace

QByteArray EnergyVad::trimSilence(const QByteArray &pcm, const Settings &settings) {
    const qsizetype frameSamples = qsizetype(settings.sampleRate) * settings.frameMs / 1000;
    const std::vector<double> levels = frameLevels(pcm, frameSamples);
    if (levels.size() < 10) return pcm;
    std::vector<double> sorted = levels;
    std::sort(sorted.begin(), sorted.end());
    const double threshold = std::max(settings.minLevelDb, sorted[sorted.size() / 10] + settings.stopMarginDb);
    const auto loud = [threshold](double level) { return level >= threshold; };
    const auto first = std::find_if(levels.begin(), levels.end(), loud);
    if (first == levels.end()) return pcm;
    const auto last = std::find_if(levels.rbegin(), levels.rend(), loud);

    const qsizetype frameBytes = frameSamples * 2;
    const qsizetype before = settings.preRollMs / settings.frameMs;
    const qsizetype after = settings.trailingMs / settings.frameMs + 5;
    const qsizetype begin = std::max<qsizetype>(0, (first - levels.begin()) - before) * frameBytes;
    const qsizetype end = std::min<qsizetype>(qsizetype(levels.size()), (levels.rend() - last) + after) * frameBytes;
    return pcm.mid(begin, end - begin);
}

bool EnergyVad::containsSpeech(QByteArrayView pcm) {
    return containsSpeech(pcm, Settings{});
}

bool EnergyVad::containsSpeech(QByteArrayView pcm, const Settings &settings) {
    const qsizetype frameSamples = qsizetype(settings.sampleRate) * settings.frameMs / 1000;
    std::vector<double> levels = frameLevels(pcm, frameSamples);
    if (qsizetype(levels.size()) * settings.frameMs < settings.minSpeechMs) return false;
    std::sort(levels.begin(), levels.end());
    const double quiet = levels[levels.size() / 10];
    const double loud = levels[levels.size() * 95 / 100];
    // Loud throughout (the key was held exactly while talking) or loud parts above quiet ones.
    constexpr double kClearlyLoudDb = -40.0;
    return loud >= kClearlyLoudDb || (loud >= settings.minLevelDb && loud - quiet >= settings.stopMarginDb);
}

double EnergyVad::noiseFloorDb() const {
    return recentLevels_.empty() ? -100.0 : *std::min_element(recentLevels_.begin(), recentLevels_.end());
}

void EnergyVad::reset() {
    pending_.clear();
    preRoll_.clear();
    candidate_.clear();
    utterance_.clear();
    finished_.clear();
    recentLevels_.clear();
    levelDb_ = -100.0;
    inSpeech_ = false;
    heardSpeech_ = false;
    loudMs_ = 0;
    quietMs_ = 0;
    speechMs_ = 0;
}

QByteArray EnergyVad::takeUtterance() {
    return std::exchange(finished_, {});
}

QList<EnergyVad::Event> EnergyVad::feed(QByteArrayView pcm) {
    QList<Event> events;
    pending_.append(pcm.data(), pcm.size());
    const qsizetype frameBytes = frameSamples_ * 2;
    if (frameBytes <= 0) return events;

    qsizetype offset = 0;
    QByteArray frame(frameBytes, Qt::Uninitialized);
    for (; offset + frameBytes <= pending_.size(); offset += frameBytes) {
        // Copy: the frame must be suitably aligned for qint16 access.
        std::copy_n(pending_.constData() + offset, frameBytes, frame.data());
        processFrame(reinterpret_cast<const qint16 *>(frame.constData()), events);
    }
    pending_.remove(0, offset);
    return events;
}

void EnergyVad::processFrame(const qint16 *samples, QList<Event> &events) {
    const QByteArrayView frame(reinterpret_cast<const char *>(samples), frameSamples_ * 2);
    levelDb_ = rmsDb(samples, frameSamples_);

    recentLevels_.push_back(levelDb_);
    const auto window = size_t(std::max(1, settings_.floorWindowMs / settings_.frameMs));
    while (recentLevels_.size() > window) recentLevels_.pop_front();
    const double floor = noiseFloorDb();
    const double startLevel = std::max(settings_.minLevelDb, floor + settings_.startMarginDb);
    const double stopLevel = std::max(settings_.minLevelDb, floor + settings_.stopMarginDb);

    if (!inSpeech_) {
        if (levelDb_ >= startLevel) {
            loudMs_ += settings_.frameMs;
            candidate_.append(frame);
            if (loudMs_ < settings_.minSpeechMs) return;
            inSpeech_ = true;
            heardSpeech_ = true;
            utterance_ = preRoll_ + candidate_;
            preRoll_.clear();
            candidate_.clear();
            speechMs_ = loudMs_;
            quietMs_ = 0;
            events.append(Event::SpeechStarted);
            return;
        }
        // Too short to be speech: the burst becomes part of the pre-roll.
        preRoll_.append(candidate_);
        candidate_.clear();
        loudMs_ = 0;
        preRoll_.append(frame);
        const qsizetype keep = bytesFor(settings_.preRollMs);
        if (preRoll_.size() > keep) preRoll_.remove(0, preRoll_.size() - keep);
        return;
    }

    utterance_.append(frame);
    speechMs_ += settings_.frameMs;
    quietMs_ = levelDb_ >= stopLevel ? 0 : quietMs_ + settings_.frameMs;
    if (quietMs_ >= settings_.hangoverMs || speechMs_ >= settings_.maxUtteranceMs) endUtterance(events);
}

void EnergyVad::endUtterance(QList<Event> &events) {
    // Drop most of the trailing quiet; recognisers do not need it.
    const qsizetype trailing = bytesFor(std::max(0, quietMs_ - settings_.trailingMs));
    utterance_.chop(std::min(trailing, utterance_.size()));
    finished_ = std::exchange(utterance_, {});
    inSpeech_ = false;
    loudMs_ = 0;
    quietMs_ = 0;
    speechMs_ = 0;
    events.append(Event::SpeechEnded);
}

} // namespace nexa

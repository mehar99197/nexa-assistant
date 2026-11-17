#include "voice/listen/SileroVoiceDetector.h"

#include <QFile>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kSampleRate = 16'000;
constexpr qsizetype kWindow = 512;                          // what Silero takes at 16 kHz
constexpr float kSileroPauseSeconds = 0.1f;                 // Silero's own: our pause is counted on top
constexpr float kSileroShortestSeconds = 0.25f;             // speech shorter than this is not speech
// Silero says "voice" once 0.25 s of it were heard; the utterance keeps 0.3 s before that, like EnergyVad.
constexpr qint64 kOnsetBefore = qint64(kSampleRate * (0.25 + 0.3)) + kWindow;
constexpr qint64 kSileroLag = qint64(kSampleRate * 0.1);    // "no voice" comes 0.1 s after the voice stopped
constexpr qint64 kTrailing = kSampleRate / 5;               // quiet kept after the speech: 200 ms

} // namespace

Result<std::unique_ptr<SileroVoiceDetector>> SileroVoiceDetector::create(SherpaRuntime *runtime, float threshold) {
    if (!runtime || !runtime->load()) {
        return Error{ErrorCode::NotFound, runtime ? runtime->problem() : u"No sherpa-onnx."_s};
    }
    const QByteArray model = QFile::encodeName(runtime->sileroModel());
    SherpaOnnxVadModelConfig config{};
    config.silero_vad.model = model.constData();
    config.silero_vad.threshold = threshold;
    config.silero_vad.min_silence_duration = kSileroPauseSeconds;
    config.silero_vad.min_speech_duration = kSileroShortestSeconds;
    config.silero_vad.window_size = int32_t(kWindow);
    config.silero_vad.max_speech_duration = 60.0f;          // the longest utterance is cut here, not by Silero
    config.sample_rate = kSampleRate;
    config.num_threads = 1;
    config.provider = "cpu";
    const SherpaOnnxVoiceActivityDetector *vad = runtime->createVad(&config, 30.0f);
    if (!vad) return Error{ErrorCode::Unknown, u"The Silero voice detector did not start."_s};
    return std::unique_ptr<SileroVoiceDetector>(new SileroVoiceDetector(runtime, vad));
}

SileroVoiceDetector::SileroVoiceDetector(SherpaRuntime *runtime, const SherpaOnnxVoiceActivityDetector *vad)
    : runtime_(runtime), vad_(vad) {}

SileroVoiceDetector::~SileroVoiceDetector() {
    runtime_->destroyVad(vad_);
}

QList<IVoiceDetector::Event> SileroVoiceDetector::feed(QByteArrayView pcm16k) {
    const qsizetype count = pcm16k.size() / 2;
    std::vector<qint16> input(static_cast<size_t>(count));
    std::memcpy(input.data(), pcm16k.data(), size_t(count) * 2);
    for (const qint16 sample : input) pending_.push_back(float(sample) / 32768.0f);

    QList<Event> events;
    size_t used = 0;
    for (; used + size_t(kWindow) <= pending_.size(); used += size_t(kWindow)) window(pending_.data() + used, events);
    pending_.erase(pending_.begin(), pending_.begin() + qsizetype(used));
    return events;
}

void SileroVoiceDetector::window(const float *samples, QList<Event> &events) {
    runtime_->vadAcceptWaveform(vad_, samples, int32_t(kWindow));
    fed_ += kWindow;
    const bool voice = runtime_->vadDetected(vad_) != 0;
    // Silero's own segments are not used: its pauses are short on purpose.
    while (!runtime_->vadEmpty(vad_)) {
        runtime_->destroySpeechSegment(runtime_->vadFront(vad_));
        runtime_->vadPop(vad_);
    }

    if (!inUtterance_) {
        if (!voice) return;
        inUtterance_ = true;
        start_ = std::max<qint64>(0, fed_ - kOnsetBefore);
        quietSince_ = -1;
        events.append(Event{.kind = Event::Kind::Started, .start = start_, .end = 0});
        return;
    }
    if (voice) {
        quietSince_ = -1;
    } else if (quietSince_ < 0) {
        quietSince_ = fed_ - kSileroLag;
    }
    const bool paused = quietSince_ >= 0 && fed_ - quietSince_ >= endPause_;
    if (!paused && fed_ - start_ < longest_) return;
    const qint64 end = paused ? std::min(fed_, quietSince_ + kTrailing) : fed_;
    events.append(Event{.kind = Event::Kind::Ended, .start = start_, .end = end});
    inUtterance_ = false;
    quietSince_ = -1;
}

void SileroVoiceDetector::reset() {
    runtime_->vadReset(vad_);
    pending_.clear();
    fed_ = 0;
    inUtterance_ = false;
    quietSince_ = -1;
}

} // namespace nexa

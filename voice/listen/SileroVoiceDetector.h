#pragma once

#include "core/Result.h"
#include "voice/listen/IVoiceDetector.h"
#include "voice/wake/SherpaRuntime.h"

#include <memory>
#include <vector>

namespace nexa {

// Speech by a model that hears voices (IVoiceDetector): Silero VAD through sherpa-onnx, so a fan, a hum or a
// door neither starts an utterance nor keeps one going. Silero says, 32 ms at a time, whether someone speaks;
// the pause that ends an utterance and the longest one are applied here, so they can change mid-utterance (a
// request after the name waits longer than the room's sentences). Under 1 % of one core; runs on the front's
// thread.
class SileroVoiceDetector : public IVoiceDetector {
public:
    static Result<std::unique_ptr<SileroVoiceDetector>> create(SherpaRuntime *runtime, float threshold);
    ~SileroVoiceDetector() override;

    QString name() const override { return QStringLiteral("Silero"); }
    QList<Event> feed(QByteArrayView pcm16k) override;
    void reset() override;
    void setEndPauseMs(int ms) override { endPause_ = qint64(ms) * 16; }
    void setMaxUtteranceMs(int ms) override { longest_ = qint64(ms) * 16; }

private:
    SileroVoiceDetector(SherpaRuntime *runtime, const SherpaOnnxVoiceActivityDetector *vad);
    void window(const float *samples, QList<Event> &events);

    SherpaRuntime *runtime_;
    const SherpaOnnxVoiceActivityDetector *vad_;
    std::vector<float> pending_;        // samples short of a whole window
    qint64 fed_{0};                     // samples given to Silero since reset()
    bool inUtterance_{false};
    qint64 start_{0};
    qint64 quietSince_{-1};             // where Silero last stopped hearing a voice
    qint64 endPause_{700 * 16};
    qint64 longest_{8000 * 16};
};

} // namespace nexa

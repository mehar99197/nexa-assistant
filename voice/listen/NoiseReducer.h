#pragma once

#include "core/Result.h"
#include "voice/listen/IAudioFilter.h"
#include "voice/wake/SherpaRuntime.h"

#include <memory>

namespace nexa {

// Background noise taken out of the microphone's audio before the voice detector and the spotter hear it
// (IAudioFilter): sherpa-onnx's streaming denoiser with GTCRN. With a fan as loud as the voice it let Silero
// find every sentence and the spotter catch more names; the recognisers still get the raw audio (it did not help
// whisper). About a fifth of one core; runs on the front's thread.
class NoiseReducer : public IAudioFilter {
public:
    static Result<std::unique_ptr<NoiseReducer>> create(SherpaRuntime *runtime);
    ~NoiseReducer() override;

    QByteArray process(QByteArrayView pcm16k) override;
    void reset() override;

private:
    NoiseReducer(SherpaRuntime *runtime, const SherpaOnnxOnlineSpeechDenoiser *denoiser);

    SherpaRuntime *runtime_;
    const SherpaOnnxOnlineSpeechDenoiser *denoiser_;
};

} // namespace nexa

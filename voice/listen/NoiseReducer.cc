#include "voice/listen/NoiseReducer.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kSampleRate = 16'000;

} // namespace

Result<std::unique_ptr<NoiseReducer>> NoiseReducer::create(SherpaRuntime *runtime) {
    if (!runtime || !runtime->load()) {
        return Error{ErrorCode::NotFound, runtime ? runtime->problem() : u"No sherpa-onnx."_s};
    }
    const QByteArray model = QFile::encodeName(runtime->gtcrnModel());
    SherpaOnnxOnlineSpeechDenoiserConfig config{};
    config.model.gtcrn.model = model.constData();
    config.model.num_threads = 1;
    config.model.provider = "cpu";
    const SherpaOnnxOnlineSpeechDenoiser *denoiser = runtime->createDenoiser(&config);
    if (!denoiser) return Error{ErrorCode::Unknown, u"The noise reducer did not start."_s};
    return std::unique_ptr<NoiseReducer>(new NoiseReducer(runtime, denoiser));
}

NoiseReducer::NoiseReducer(SherpaRuntime *runtime, const SherpaOnnxOnlineSpeechDenoiser *denoiser)
    : runtime_(runtime), denoiser_(denoiser) {}

NoiseReducer::~NoiseReducer() {
    runtime_->destroyDenoiser(denoiser_);
}

QByteArray NoiseReducer::process(QByteArrayView pcm16k) {
    const qsizetype count = pcm16k.size() / 2;
    std::vector<qint16> input(static_cast<size_t>(count));
    std::memcpy(input.data(), pcm16k.data(), size_t(count) * 2);
    std::vector<float> samples(static_cast<size_t>(count));
    for (size_t i = 0; i < input.size(); ++i) samples[i] = float(input[i]) / 32768.0f;

    const SherpaOnnxDenoisedAudio *audio = runtime_->runDenoiser(denoiser_, samples.data(), int32_t(count), kSampleRate);
    if (!audio) return {};      // still filling its first frame
    QByteArray cleaned;
    if (audio->sample_rate == kSampleRate) {
        cleaned.resize(qsizetype(audio->n) * 2);
        auto *out = reinterpret_cast<qint16 *>(cleaned.data());
        for (int32_t i = 0; i < audio->n; ++i) {
            out[i] = qint16(std::lround(std::clamp(audio->samples[i], -1.0f, 1.0f) * 32767.0f));
        }
    } else {
        cleaned = QByteArray(pcm16k.data(), count * 2);     // another model's rate: hear it as it was
    }
    runtime_->destroyDenoisedAudio(audio);
    return cleaned;
}

void NoiseReducer::reset() {
    runtime_->resetDenoiser(denoiser_);
}

} // namespace nexa

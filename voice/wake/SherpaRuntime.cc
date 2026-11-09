#include "voice/wake/SherpaRuntime.h"

#include <QFileInfo>
#include <QStandardPaths>

#include <type_traits>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QString kInstall = u"run scripts/download-sherpa.sh"_s;

} // namespace

SherpaRuntime::SherpaRuntime(QString dir) : dir_(std::move(dir)) {}

QString SherpaRuntime::defaultDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa/sherpa"_s;
}

QString SherpaRuntime::kwsDir() const {
    return dir_ + u"/kws"_s;
}

QString SherpaRuntime::sileroModel() const {
    return dir_ + u"/silero_vad.onnx"_s;
}

QString SherpaRuntime::gtcrnModel() const {
    return dir_ + u"/gtcrn_simple.onnx"_s;
}

QString SherpaRuntime::speakerModel() const {
    return dir_ + u"/speaker/3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx"_s;
}

bool SherpaRuntime::load() {
    if (attempted_) return loaded_;
    attempted_ = true;

    const QString path = dir_ + u"/lib/libsherpa-onnx-c-api.so"_s;
    const QStringList models{kwsDir() + u"/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s,
                             kwsDir() + u"/decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s,
                             kwsDir() + u"/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s,
                             kwsDir() + u"/tokens.txt"_s, kwsDir() + u"/bpe.model"_s, sileroModel(), gtcrnModel()};
    if (!QFileInfo::exists(path)) {
        problem_ = u"Instant spotting is not installed — %1."_s.arg(kInstall);
        return false;
    }
    for (const QString &model : models) {
        if (!QFileInfo::exists(model)) {
            problem_ = u"%1 is missing — %2."_s.arg(QFileInfo(model).fileName(), kInstall);
            return false;
        }
    }
    library_.setFileName(path);
    if (!library_.load()) {
        problem_ = u"Cannot load sherpa-onnx (%1) — %2."_s.arg(library_.errorString(), kInstall);
        return false;
    }

    QString missing;
    const auto resolve = [this, &missing](auto &function, const char *name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(library_.resolve(name));
        if (!function && missing.isEmpty()) missing = QString::fromLatin1(name);
    };
    resolve(createKeywordSpotter, "SherpaOnnxCreateKeywordSpotter");
    resolve(destroyKeywordSpotter, "SherpaOnnxDestroyKeywordSpotter");
    resolve(createKeywordStream, "SherpaOnnxCreateKeywordStream");
    resolve(destroyOnlineStream, "SherpaOnnxDestroyOnlineStream");
    resolve(acceptWaveform, "SherpaOnnxOnlineStreamAcceptWaveform");
    resolve(isKeywordStreamReady, "SherpaOnnxIsKeywordStreamReady");
    resolve(decodeKeywordStream, "SherpaOnnxDecodeKeywordStream");
    resolve(getKeywordResult, "SherpaOnnxGetKeywordResult");
    resolve(destroyKeywordResult, "SherpaOnnxDestroyKeywordResult");
    resolve(resetKeywordStream, "SherpaOnnxResetKeywordStream");
    resolve(createVad, "SherpaOnnxCreateVoiceActivityDetector");
    resolve(destroyVad, "SherpaOnnxDestroyVoiceActivityDetector");
    resolve(vadAcceptWaveform, "SherpaOnnxVoiceActivityDetectorAcceptWaveform");
    resolve(vadEmpty, "SherpaOnnxVoiceActivityDetectorEmpty");
    resolve(vadDetected, "SherpaOnnxVoiceActivityDetectorDetected");
    resolve(vadFront, "SherpaOnnxVoiceActivityDetectorFront");
    resolve(vadPop, "SherpaOnnxVoiceActivityDetectorPop");
    resolve(destroySpeechSegment, "SherpaOnnxDestroySpeechSegment");
    resolve(vadReset, "SherpaOnnxVoiceActivityDetectorReset");
    resolve(createDenoiser, "SherpaOnnxCreateOnlineSpeechDenoiser");
    resolve(destroyDenoiser, "SherpaOnnxDestroyOnlineSpeechDenoiser");
    resolve(runDenoiser, "SherpaOnnxOnlineSpeechDenoiserRun");
    resolve(resetDenoiser, "SherpaOnnxOnlineSpeechDenoiserReset");
    resolve(destroyDenoisedAudio, "SherpaOnnxDestroyDenoisedAudio");
    if (!missing.isEmpty()) {
        problem_ = u"The installed sherpa-onnx has no %1 (another version) — %2."_s.arg(missing, kInstall);
        library_.unload();
        return false;
    }
    // Optional: telling the owner's voice from others. Missing, everything else still works.
    QString speakerMissing;
    const auto optional = [this, &speakerMissing](auto &function, const char *name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(library_.resolve(name));
        if (!function && speakerMissing.isEmpty()) speakerMissing = QString::fromLatin1(name);
    };
    optional(createSpeakerExtractor, "SherpaOnnxCreateSpeakerEmbeddingExtractor");
    optional(destroySpeakerExtractor, "SherpaOnnxDestroySpeakerEmbeddingExtractor");
    optional(speakerDim, "SherpaOnnxSpeakerEmbeddingExtractorDim");
    optional(speakerCreateStream, "SherpaOnnxSpeakerEmbeddingExtractorCreateStream");
    optional(speakerIsReady, "SherpaOnnxSpeakerEmbeddingExtractorIsReady");
    optional(speakerCompute, "SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding");
    optional(speakerDestroyEmbedding, "SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding");
    optional(inputFinished, "SherpaOnnxOnlineStreamInputFinished");
    speakers_ = speakerMissing.isEmpty() && QFileInfo::exists(speakerModel());
    loaded_ = true;
    problem_.clear();
    return true;
}

} // namespace nexa

#include "voice/wake/SherpaRuntime.h"

#include <QDir>
#include <QFileInfo>
#include "core/config/Portable.h"

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <type_traits>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QString kInstall = u"run scripts/download-sherpa.sh"_s;

} // namespace

SherpaRuntime::SherpaRuntime(QString modelsDir, QString libraryDir)
    : dir_(std::move(modelsDir)), libraryDir_(libraryDir.isEmpty() ? dir_ : std::move(libraryDir)) {}

QString SherpaRuntime::defaultDir() {
    return portable::dataDir() + u"/sherpa"_s;
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

QString SherpaRuntime::zipVoiceDir() const {
    return dir_ + u"/zipvoice"_s;
}

bool SherpaRuntime::load() {
    if (attempted_) return loaded_;
    attempted_ = true;

    const QString path = [] (const QString &base) -> QString {
#ifdef Q_OS_WIN
        const QString libPath = base + u"/lib/sherpa-onnx-c-api.dll"_s;
        if (QFileInfo::exists(libPath)) return libPath;
        const QString binPath = base + u"/bin/sherpa-onnx-c-api.dll"_s;
        if (QFileInfo::exists(binPath)) return binPath;
        return base + u"/sherpa-onnx-c-api.dll"_s;
#else
        return base + u"/lib/libsherpa-onnx-c-api.so"_s;
#endif
    }(libraryDir_);
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
#ifdef Q_OS_WIN
    const QString nativeLibDir = QDir::toNativeSeparators(QFileInfo(path).absolutePath());
    ::SetDllDirectoryW(reinterpret_cast<LPCWSTR>(nativeLibDir.utf16()));
    const bool loadedLib = library_.load();
    ::SetDllDirectoryW(nullptr);
    if (!loadedLib) {
#else
    if (!library_.load()) {
#endif
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
    // Optional: a voice from a sample (ZipVoice). Missing, Piper and the cloud voices still speak.
    QString ttsMissing;
    const auto optionalTts = [this, &ttsMissing](auto &function, const char *name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(library_.resolve(name));
        if (!function && ttsMissing.isEmpty()) ttsMissing = QString::fromLatin1(name);
    };
    optionalTts(createOfflineTts, "SherpaOnnxCreateOfflineTts");
    optionalTts(destroyOfflineTts, "SherpaOnnxDestroyOfflineTts");
    optionalTts(offlineTtsSampleRate, "SherpaOnnxOfflineTtsSampleRate");
    optionalTts(offlineTtsGenerateWithConfig, "SherpaOnnxOfflineTtsGenerateWithConfig");
    optionalTts(destroyGeneratedAudio, "SherpaOnnxDestroyOfflineTtsGeneratedAudio");
    tts_ = ttsMissing.isEmpty();
    for (const QString &file : {u"encoder.int8.onnx"_s, u"decoder.int8.onnx"_s, u"vocos_24khz.onnx"_s, u"tokens.txt"_s, u"lexicon.txt"_s}) {
        if (!QFileInfo::exists(zipVoiceDir() + u'/' + file)) tts_ = false;
    }
    loaded_ = true;
    problem_.clear();
    return true;
}

} // namespace nexa

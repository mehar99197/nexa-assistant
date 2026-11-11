#pragma once

#include "voice/wake/sherpa/c-api.h"

#include <QLibrary>
#include <QString>

namespace nexa {

// sherpa-onnx's C API (v2.0.1), loaded at run time from <dir>/lib (Facade): nothing is linked, so Nexa builds
// and runs without it, and scripts/download-sherpa.sh puts the library and its models in place. The functions
// are resolved by name against the vendored header (voice/wake/sherpa/c-api.h, Apache-2.0); one missing means
// another version — not used. Made once in main.cc; the parts it serves may run on other threads.
class SherpaRuntime {
public:
    explicit SherpaRuntime(QString dir = defaultDir());

    static QString defaultDir();    // ~/.local/share/nexa/sherpa

    // Loads the library and checks the models are there; once — later calls give the same answer.
    bool load();
    bool available() const { return loaded_; }
    QString problem() const { return problem_; }

    QString kwsDir() const;         // the keyword spotter: encoder, decoder, joiner, tokens.txt, bpe.model
    QString sileroModel() const;
    QString gtcrnModel() const;
    QString speakerModel() const;   // the speaker-embedding model (speaker/…onnx)
    QString zipVoiceDir() const;    // ZipVoice: encoder, decoder, tokens, lexicon and the Vocos vocoder (zipvoice/)
    // The owner's voice can be told: the library has the speaker-embedding functions and the model is installed.
    // Optional — without it the spotter, Silero and the noise reducer still work.
    bool canTellSpeakers() const { return loaded_ && speakers_; }
    // Nexa can speak in a voice from a sample: the library has the TTS functions and ZipVoice is installed. Optional too.
    bool canCloneVoice() const { return loaded_ && tts_; }

    // The keyword spotter.
    decltype(&SherpaOnnxCreateKeywordSpotter) createKeywordSpotter{nullptr};
    decltype(&SherpaOnnxDestroyKeywordSpotter) destroyKeywordSpotter{nullptr};
    decltype(&SherpaOnnxCreateKeywordStream) createKeywordStream{nullptr};
    decltype(&SherpaOnnxDestroyOnlineStream) destroyOnlineStream{nullptr};
    decltype(&SherpaOnnxOnlineStreamAcceptWaveform) acceptWaveform{nullptr};
    decltype(&SherpaOnnxIsKeywordStreamReady) isKeywordStreamReady{nullptr};
    decltype(&SherpaOnnxDecodeKeywordStream) decodeKeywordStream{nullptr};
    decltype(&SherpaOnnxGetKeywordResult) getKeywordResult{nullptr};
    decltype(&SherpaOnnxDestroyKeywordResult) destroyKeywordResult{nullptr};
    decltype(&SherpaOnnxResetKeywordStream) resetKeywordStream{nullptr};
    // Voice activity detection (Silero).
    decltype(&SherpaOnnxCreateVoiceActivityDetector) createVad{nullptr};
    decltype(&SherpaOnnxDestroyVoiceActivityDetector) destroyVad{nullptr};
    decltype(&SherpaOnnxVoiceActivityDetectorAcceptWaveform) vadAcceptWaveform{nullptr};
    decltype(&SherpaOnnxVoiceActivityDetectorEmpty) vadEmpty{nullptr};
    decltype(&SherpaOnnxVoiceActivityDetectorDetected) vadDetected{nullptr};
    decltype(&SherpaOnnxVoiceActivityDetectorFront) vadFront{nullptr};
    decltype(&SherpaOnnxVoiceActivityDetectorPop) vadPop{nullptr};
    decltype(&SherpaOnnxDestroySpeechSegment) destroySpeechSegment{nullptr};
    decltype(&SherpaOnnxVoiceActivityDetectorReset) vadReset{nullptr};
    // Noise reduction (GTCRN, streaming).
    decltype(&SherpaOnnxCreateOnlineSpeechDenoiser) createDenoiser{nullptr};
    decltype(&SherpaOnnxDestroyOnlineSpeechDenoiser) destroyDenoiser{nullptr};
    decltype(&SherpaOnnxOnlineSpeechDenoiserRun) runDenoiser{nullptr};
    decltype(&SherpaOnnxOnlineSpeechDenoiserReset) resetDenoiser{nullptr};
    decltype(&SherpaOnnxDestroyDenoisedAudio) destroyDenoisedAudio{nullptr};
    // Speaker embeddings (whose voice it is).
    decltype(&SherpaOnnxCreateSpeakerEmbeddingExtractor) createSpeakerExtractor{nullptr};
    decltype(&SherpaOnnxDestroySpeakerEmbeddingExtractor) destroySpeakerExtractor{nullptr};
    decltype(&SherpaOnnxSpeakerEmbeddingExtractorDim) speakerDim{nullptr};
    decltype(&SherpaOnnxSpeakerEmbeddingExtractorCreateStream) speakerCreateStream{nullptr};
    decltype(&SherpaOnnxSpeakerEmbeddingExtractorIsReady) speakerIsReady{nullptr};
    decltype(&SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding) speakerCompute{nullptr};
    decltype(&SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding) speakerDestroyEmbedding{nullptr};
    decltype(&SherpaOnnxOnlineStreamInputFinished) inputFinished{nullptr};
    // Text to speech (ZipVoice, a voice from a sample).
    decltype(&SherpaOnnxCreateOfflineTts) createOfflineTts{nullptr};
    decltype(&SherpaOnnxDestroyOfflineTts) destroyOfflineTts{nullptr};
    decltype(&SherpaOnnxOfflineTtsSampleRate) offlineTtsSampleRate{nullptr};
    decltype(&SherpaOnnxOfflineTtsGenerateWithConfig) offlineTtsGenerateWithConfig{nullptr};
    decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio) destroyGeneratedAudio{nullptr};

private:
    QString dir_;
    QLibrary library_;
    bool attempted_{false};
    bool loaded_{false};
    bool speakers_{false};
    bool tts_{false};
    QString problem_;
};

} // namespace nexa

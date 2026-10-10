#include "voice/VoiceBackends.h"

#include "voice/stt/CloudRecognizer.h"
#include "voice/stt/RecognizerChain.h"
#include "voice/tts/CloudSynthesizer.h"
#include "voice/tts/PiperSynthesizer.h"
#include "voice/tts/SpeechdTtsBackend.h"
#include "voice/tts/SynthesizedSpeech.h"
#ifdef NEXA_HAVE_WHISPER
#include "voice/stt/WhisperRecognizer.h"
#endif

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// The active provider's endpoint, when it can serve `model` (OpenAI-compatible, key present).
bool cloudUsable(const ProviderConfig *provider, const QString &model) {
    return provider && provider->kind == kKindOpenAiCompatible && !provider->baseUrl.trimmed().isEmpty()
        && (!provider->requiresKey || !provider->apiKey.isEmpty()) && !model.trimmed().isEmpty();
}

QString baseUrl(const ProviderConfig &provider) {
    QString url = provider.baseUrl.trimmed();
    while (url.endsWith(u'/')) url.chop(1);
    return url;
}

QString piperDir() {
    return VoiceBackends::dataDir() + u"/piper"_s;
}

} // namespace

QString VoiceBackends::dataDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa"_s;
}

QString VoiceBackends::whisperModelPath(const QString &model) {
    return dataDir() + u"/models/ggml-%1.bin"_s.arg(model);
}

QString VoiceBackends::piperBinary() {
    return piperDir() + u"/piper"_s;
}

QStringList VoiceBackends::piperVoices() {
    QStringList voices;
    const QDir dir(piperDir() + u"/voices"_s);
    for (const QFileInfo &file : dir.entryInfoList({u"*.onnx"_s}, QDir::Files, QDir::Name)) {
        if (QFileInfo::exists(file.filePath() + u".json"_s)) voices.append(file.completeBaseName());
    }
    return voices;
}

bool VoiceBackends::whisperBuilt() {
#ifdef NEXA_HAVE_WHISPER
    return true;
#else
    return false;
#endif
}

Result<std::unique_ptr<ISpeechRecognizer>> VoiceBackends::createLocalRecognizer(const VoiceConfig &voice) {
#ifdef NEXA_HAVE_WHISPER
    const QString path = whisperModelPath(voice.whisperModel);
    if (!QFileInfo::exists(path)) {
        return Error{ErrorCode::NotFound,
                     u"the whisper model is not installed (run scripts/download-whisper-model.sh %1)"_s.arg(
                         voice.whisperModel)};
    }
    return std::unique_ptr<ISpeechRecognizer>(std::make_unique<WhisperRecognizer>(path, voice.language));
#else
    Q_UNUSED(voice)
    return Error{ErrorCode::NotSupported, u"this build has no local speech recognition (NEXA_BUILD_WHISPER)"_s};
#endif
}

Result<std::unique_ptr<ISpeechRecognizer>> VoiceBackends::createRecognizer(const VoiceConfig &voice,
                                                                            const ProviderConfig *provider,
                                                                            HttpClient *http) {
    const bool wantCloud = voice.sttBackend != u"whisper"_s;
    const bool wantLocal = voice.sttBackend != u"cloud"_s;
    std::vector<std::unique_ptr<ISpeechRecognizer>> chain;
    QStringList problems;

    if (wantCloud) {
        if (cloudUsable(provider, provider ? provider->sttModel : QString())) {
            chain.push_back(std::make_unique<CloudRecognizer>(
                http, CloudRecognizer::Settings{.providerName = provider->name,
                                                .baseUrl = baseUrl(*provider),
                                                .apiKey = provider->apiKey,
                                                .model = provider->sttModel,
                                                .language = voice.language}));
        } else {
            problems.append(u"the active provider has no speech recognition model"_s);
        }
    }
    if (wantLocal) {
        Result<std::unique_ptr<ISpeechRecognizer>> local = createLocalRecognizer(voice);
        if (local) {
            chain.push_back(std::move(local).value());
        } else {
            problems.append(local.error().message);
        }
    }

    if (chain.empty()) {
        return Error{ErrorCode::NotFound, u"No speech recognition: %1."_s.arg(problems.join(u"; "_s))};
    }
    if (chain.size() == 1) return std::move(chain.front());
    return std::unique_ptr<ISpeechRecognizer>(std::make_unique<RecognizerChain>(std::move(chain)));
}

Result<std::unique_ptr<ITextToSpeech>> VoiceBackends::createTextToSpeech(const VoiceConfig &voice,
                                                                          const ProviderConfig *provider,
                                                                          HttpClient *http) {
    const QString backend = voice.ttsBackend;
    if (backend == u"off"_s) return std::unique_ptr<ITextToSpeech>();
    if (backend == u"speechd"_s) {
        return std::unique_ptr<ITextToSpeech>(std::make_unique<SpeechdTtsBackend>(voice.speechdVoice, voice.ttsRate));
    }

    const bool automatic = backend != u"cloud"_s && backend != u"piper"_s;
    std::vector<std::unique_ptr<ISpeechSynthesizer>> chain;
    QStringList problems;

    if (automatic || backend == u"cloud"_s) {
        if (cloudUsable(provider, provider ? provider->ttsModel : QString())) {
            chain.push_back(std::make_unique<CloudSynthesizer>(
                http, CloudSynthesizer::Settings{.providerName = provider->name,
                                                 .baseUrl = baseUrl(*provider),
                                                 .apiKey = provider->apiKey,
                                                 .model = provider->ttsModel,
                                                 .voice = provider->ttsVoice,
                                                 .rate = voice.ttsRate}));
        } else {
            problems.append(u"the active provider has no speech model"_s);
        }
    }
    if (automatic || backend == u"piper"_s) {
        const QStringList voices = piperVoices();
        const QString name = voices.contains(voice.piperVoice) ? voice.piperVoice : voices.value(0);
        if (QFileInfo(piperBinary()).isExecutable() && !name.isEmpty()) {
            chain.push_back(std::make_unique<PiperSynthesizer>(
                piperBinary(), piperDir() + u"/voices/%1.onnx"_s.arg(name), voice.ttsRate));
        } else {
            problems.append(u"Piper is not installed (run scripts/download-piper.sh)"_s);
        }
    }

    if (!chain.empty()) return std::unique_ptr<ITextToSpeech>(std::make_unique<SynthesizedSpeech>(std::move(chain)));
    if (automatic) {
        return std::unique_ptr<ITextToSpeech>(std::make_unique<SpeechdTtsBackend>(voice.speechdVoice, voice.ttsRate));
    }
    return Error{ErrorCode::NotFound, u"No speech output: %1."_s.arg(problems.join(u"; "_s))};
}

} // namespace nexa

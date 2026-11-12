#include "voice/VoiceBackends.h"

#include "voice/EngineLocator.h"

#include "voice/listen/EnergyVoiceDetector.h"
#include "voice/listen/NoiseReducer.h"
#include "voice/listen/SileroVoiceDetector.h"
#include "voice/stt/CloudRecognizer.h"
#include "voice/stt/RecognizerChain.h"
#include "voice/tts/ClonedVoiceSynthesizer.h"
#include "voice/tts/CloudSynthesizer.h"
#include "voice/tts/PiperSynthesizer.h"
#include "voice/tts/SpeechdTtsBackend.h"
#include "voice/tts/SynthesizedSpeech.h"
#include "voice/tts/VoiceSample.h"
#include "voice/wake/KeywordSpotter.h"
#include "voice/wake/SherpaRuntime.h"
#include "voice/wake/WakePhrases.h"
#include "voice/wake/WordPieces.h"
#ifdef NEXA_HAVE_WHISPER
#include "voice/stt/WhisperRecognizer.h"
#endif

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Why a provider cannot do speech; empty when it can (an OpenAI-compatible endpoint with its key — Nexa's cloud
// voice speaks the OpenAI audio API).
QString cannotSpeak(const ProviderConfig &provider) {
    if (provider.kind == kKindAgy) return u"takes text only"_s;
    if (provider.kind == kKindOpenCode || provider.kind == kKindCodex) return u"no speech models"_s;
    if (provider.kind == kKindGemini || provider.kind == kKindAnthropic) {
        return u"Nexa's cloud voice speaks the OpenAI audio API"_s;
    }
    if (provider.kind != kKindOpenAiCompatible) return u"no speech models"_s;
    if (provider.baseUrl.trimmed().isEmpty()) return u"no address"_s;
    if (provider.requiresKey && provider.apiKey.isEmpty()) return u"no API key"_s;
    return {};
}

// The first choice, then the others in the list's order.
QList<const ProviderConfig *> inChainOrder(const Config &config) {
    QList<const ProviderConfig *> order;
    if (const ProviderConfig *first = config.active()) order.append(first);
    for (const ProviderConfig &provider : config.providers) {
        if (!order.contains(&provider)) order.append(&provider);
    }
    return order;
}

// The providers for one side (recognition: the sttModel; the voice: the ttsModel) — the chosen one only, or
// every one that can, in chain order.
QList<const ProviderConfig *> speechLinks(const Config &config, const QString &chosen,
                                          QString ProviderConfig::*model) {
    QList<const ProviderConfig *> links;
    for (const ProviderConfig *provider : inChainOrder(config)) {
        if (!chosen.isEmpty() && provider->id != chosen) continue;
        if (cannotSpeak(*provider).isEmpty() && !(provider->*model).trimmed().isEmpty()) links.append(provider);
    }
    return links;
}

QList<VoiceProviderChoice> choicesOf(const Config &config) {
    QList<VoiceProviderChoice> choices;
    for (const ProviderConfig &provider : config.providers) {
        const QString reason = cannotSpeak(provider);
        choices.append(VoiceProviderChoice{.id = provider.id,
                                           .name = provider.name.isEmpty() ? provider.id : provider.name,
                                           .usable = reason.isEmpty(),
                                           .reason = reason});
    }
    return choices;
}

QString baseUrl(const ProviderConfig &provider) {
    QString url = provider.baseUrl.trimmed();
    while (url.endsWith(u'/')) url.chop(1);
    return url;
}


} // namespace

QString VoiceBackends::dataDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa"_s;
}

QString VoiceBackends::whisperModelPath(const QString &model) {
    return dataDir() + u"/models/ggml-%1.bin"_s.arg(model);
}

QString VoiceBackends::piperDir() {
    return EngineLocator().piperDir();
}

QString VoiceBackends::piperBinary() {
    return piperDir() + u"/piper"_s;
}

QStringList VoiceBackends::piperVoices() {
    QStringList voices;
    for (const QString &folder : EngineLocator().piperVoiceSearchDirs()) {
        const QDir dir(folder);
        for (const QFileInfo &file : dir.entryInfoList({u"*.onnx"_s}, QDir::Files, QDir::Name)) {
            if (QFileInfo::exists(file.filePath() + u".json"_s) && !voices.contains(file.completeBaseName())) {
                voices.append(file.completeBaseName());
            }
        }
    }
    return voices;
}

QString VoiceBackends::piperVoicePath(const QString &name) {
    for (const QString &folder : EngineLocator().piperVoiceSearchDirs()) {
        const QString path = folder + u"/%1.onnx"_s.arg(name);
        if (QFileInfo::exists(path) && QFileInfo::exists(path + u".json"_s)) return path;
    }
    return {};
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

QList<const ProviderConfig *> VoiceBackends::recognitionProviders(const Config &config) {
    return speechLinks(config, config.voice.sttProvider, &ProviderConfig::sttModel);
}

const ProviderConfig *VoiceBackends::speechProvider(const Config &config) {
    const QList<const ProviderConfig *> links = speechLinks(config, config.voice.ttsProvider, &ProviderConfig::ttsModel);
    return links.isEmpty() ? nullptr : links.first();
}

QList<VoiceProviderChoice> VoiceBackends::recognitionChoices(const Config &config) {
    return choicesOf(config);
}

QList<VoiceProviderChoice> VoiceBackends::speechChoices(const Config &config) {
    return choicesOf(config);
}

Result<std::unique_ptr<ISpeechRecognizer>> VoiceBackends::createRecognizer(const Config &config, HttpClient *http) {
    const VoiceConfig &voice = config.voice;
    const bool wantCloud = voice.sttBackend != u"whisper"_s;
    const bool wantLocal = voice.sttBackend != u"cloud"_s;
    std::vector<std::unique_ptr<ISpeechRecognizer>> chain;
    QStringList problems;

    if (wantCloud) {
        const QList<const ProviderConfig *> providers = recognitionProviders(config);
        for (const ProviderConfig *provider : providers) {
            chain.push_back(std::make_unique<CloudRecognizer>(
                http, CloudRecognizer::Settings{.providerName = provider->name,
                                                .baseUrl = baseUrl(*provider),
                                                .apiKey = provider->apiKey,
                                                .model = provider->sttModel,
                                                .language = voice.language}));
        }
        if (providers.isEmpty()) problems.append(u"no provider has a speech recognition model"_s);
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

Result<std::unique_ptr<ISpeechSynthesizer>> VoiceBackends::createOwnVoice(const VoiceConfig &voice, SherpaRuntime *sherpa) {
    const Result<VoiceSample> sample = VoiceSample::load();
    if (!sample) return Error{ErrorCode::NotFound, u"No voice sample is recorded yet (Settings → Voice → Record my voice)."_s};
    if (!sherpa || !sherpa->load() || !sherpa->canCloneVoice()) {
        return Error{ErrorCode::NotFound, u"ZipVoice is not installed — run scripts/download-sherpa.sh."_s};
    }
    Phonemizer phonemizer(piperDir());
    if (!phonemizer.available()) return Error{ErrorCode::NotFound, phonemizer.problem()};
    return std::unique_ptr<ISpeechSynthesizer>(
        std::make_unique<ClonedVoiceSynthesizer>(sherpa, sample.value(), std::move(phonemizer), voice.ttsRate));
}

Result<std::unique_ptr<ITextToSpeech>> VoiceBackends::createTextToSpeech(const Config &config, HttpClient *http,
                                                                         SherpaRuntime *sherpa) {
    const VoiceConfig &voice = config.voice;
    const QString backend = voice.ttsBackend;
    if (backend == u"off"_s) return std::unique_ptr<ITextToSpeech>();
    if (backend == u"speechd"_s) {
        // The owner's voice still comes first when it is on and there: speech-dispatcher is what is left otherwise.
        if (voice.ownVoice) {
            if (Result<std::unique_ptr<ISpeechSynthesizer>> own = createOwnVoice(voice, sherpa)) {
                std::vector<std::unique_ptr<ISpeechSynthesizer>> chain;
                chain.push_back(std::move(own).value());
                return std::unique_ptr<ITextToSpeech>(std::make_unique<SynthesizedSpeech>(std::move(chain)));
            }
        }
        return std::unique_ptr<ITextToSpeech>(std::make_unique<SpeechdTtsBackend>(voice.speechdVoice, voice.ttsRate));
    }

    QStringList problems;
    std::vector<std::unique_ptr<ISpeechSynthesizer>> chain = createSynthesizers(config, http, &problems, sherpa);
    if (!chain.empty()) return std::unique_ptr<ITextToSpeech>(std::make_unique<SynthesizedSpeech>(std::move(chain)));
    if (backend != u"cloud"_s && backend != u"piper"_s) {
        return std::unique_ptr<ITextToSpeech>(std::make_unique<SpeechdTtsBackend>(voice.speechdVoice, voice.ttsRate));
    }
    return Error{ErrorCode::NotFound, u"No speech output: %1."_s.arg(problems.join(u"; "_s))};
}

std::vector<std::unique_ptr<ISpeechSynthesizer>> VoiceBackends::createSynthesizers(const Config &config, HttpClient *http,
                                                                                   QStringList *problemsOut,
                                                                                   SherpaRuntime *sherpa) {
    const VoiceConfig &voice = config.voice;
    const QString backend = voice.ttsBackend;
    std::vector<std::unique_ptr<ISpeechSynthesizer>> chain;
    if (backend == u"off"_s) return chain;
    const bool automatic = backend != u"cloud"_s && backend != u"piper"_s;
    QStringList problems;

    // The owner's own voice first, whatever the mode: the modes below say what speaks when it cannot.
    if (voice.ownVoice) {
        if (Result<std::unique_ptr<ISpeechSynthesizer>> own = createOwnVoice(voice, sherpa)) {
            chain.push_back(std::move(own).value());
        } else {
            problems.append(own.error().message);
        }
    }

    if (automatic || backend == u"cloud"_s) {
        if (const ProviderConfig *provider = speechProvider(config)) {
            chain.push_back(std::make_unique<CloudSynthesizer>(
                http, CloudSynthesizer::Settings{.providerName = provider->name,
                                                 .baseUrl = baseUrl(*provider),
                                                 .apiKey = provider->apiKey,
                                                 .model = provider->ttsModel,
                                                 .voice = provider->ttsVoice,
                                                 .rate = voice.ttsRate}));
        } else {
            problems.append(u"no provider has a speech model"_s);
        }
    }
    if (automatic || backend == u"piper"_s) {
        const QStringList voices = piperVoices();
        const QString name = voices.contains(voice.piperVoice) ? voice.piperVoice : voices.value(0);
        if (QFileInfo(piperBinary()).isExecutable() && !name.isEmpty()) {
            chain.push_back(std::make_unique<PiperSynthesizer>(piperBinary(), piperVoicePath(name), voice.ttsRate));
        } else {
            problems.append(u"Piper is not installed (run scripts/download-piper.sh)"_s);
        }
    }

    if (problemsOut) *problemsOut = problems;
    return chain;
}

VoiceBackends::FrontBuild VoiceBackends::createListeningFront(const VoiceConfig &voice, SherpaRuntime *sherpa,
                                                            bool withSpotter) {
    FrontBuild build;
    const SpotterTuning tuning = WakePhrases::tuning(WakePhrases::sensitivityFrom(voice.wakeSensitivity));
    ListeningFront::Parts parts;
    if (sherpa && sherpa->load()) {
        if (Result<std::unique_ptr<SileroVoiceDetector>> silero = SileroVoiceDetector::create(sherpa, tuning.sileroThreshold)) {
            parts.detector = std::move(silero).value();
        } else {
            build.problems.append(silero.error().message);
        }
        if (voice.noiseReduction) {
            if (Result<std::unique_ptr<NoiseReducer>> reducer = NoiseReducer::create(sherpa)) {
                parts.filter = std::move(reducer).value();
            } else {
                build.problems.append(reducer.error().message);
            }
        }
        if (withSpotter) {
            const Result<WordPieces> pieces = WordPieces::fromModelFile(sherpa->kwsDir() + u"/bpe.model"_s);
            const Result<QString> lines = pieces ? WakePhrases::keywordLines(voice.wakeName, pieces.value())
                                                 : Result<QString>(pieces.error());
            if (!lines) {
                build.problems.append(u"Instant spotting cannot spell the name: %1"_s.arg(lines.error().message));
            } else if (Result<std::unique_ptr<KeywordSpotter>> spotter = KeywordSpotter::create(sherpa, lines.value(), tuning)) {
                parts.spotter = std::move(spotter).value();
            } else {
                build.problems.append(spotter.error().message);
            }
        }
    } else if (sherpa && withSpotter) {
        build.problems.append(sherpa->problem());
    }
    if (!parts.detector) parts.detector = std::make_unique<EnergyVoiceDetector>(tuning.startMarginDb);
    // The models want a thread of their own; loudness alone is cheap enough to run where the audio arrives.
    const bool models = parts.filter || parts.spotter || parts.detector->name() != u"loudness"_s;
    build.front = std::make_unique<ListeningFront>(std::move(parts), models ? ListeningFront::Threading::Worker
                                                                            : ListeningFront::Threading::Inline);
    return build;
}

} // namespace nexa

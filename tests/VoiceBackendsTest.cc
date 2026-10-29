// VoiceBackends: which provider recognises requests and whose voice speaks, the voices a model has, and the
// listening front Nexa builds with — or without — sherpa-onnx.

#include "voice/VoiceBackends.h"

#include "core/config/Config.h"
#include "voice/listen/ListeningFront.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/tts/CloudVoices.h"
#include "voice/wake/SherpaRuntime.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// The owner's providers: opencode first, agy, Google Gemini without a key, Groq with a recognition model, an
// OpenAI-compatible endpoint without its key, and an Anthropic one.
Config ownersConfig() {
    Config config;
    config.providers = {
        ProviderConfig{.id = u"opencode"_s, .kind = QString(kKindOpenCode), .name = u"opencode"_s, .requiresKey = false},
        ProviderConfig{.id = u"agy"_s, .kind = QString(kKindAgy), .name = u"agy"_s, .requiresKey = false},
        ProviderConfig{.id = u"gemini"_s, .kind = QString(kKindGemini), .name = u"Google Gemini"_s,
                       .baseUrl = u"https://generativelanguage.googleapis.com/v1beta"_s},
        ProviderConfig{.id = u"groq"_s, .kind = QString(kKindOpenAiCompatible), .name = u"Groq"_s,
                       .baseUrl = u"https://api.groq.com/openai/v1"_s, .apiKey = u"gsk-test"_s,
                       .sttModel = u"whisper-large-v3-turbo"_s},
        ProviderConfig{.id = u"keyless"_s, .kind = QString(kKindOpenAiCompatible), .name = u"Keyless"_s,
                       .baseUrl = u"https://example.com/v1"_s, .sttModel = u"whisper-1"_s},
        ProviderConfig{.id = u"claude"_s, .kind = QString(kKindAnthropic), .name = u"Anthropic"_s,
                       .baseUrl = u"https://api.anthropic.com"_s, .apiKey = u"sk-test"_s},
    };
    config.activeProvider = u"opencode"_s;
    return config;
}

QStringList ids(const QList<const ProviderConfig *> &providers) {
    QStringList list;
    for (const ProviderConfig *provider : providers) list.append(provider->id);
    return list;
}

} // namespace

TEST_CASE("Recognition goes to the providers that have it, the first choice first") {
    Config config = ownersConfig();
    CHECK(ids(VoiceBackends::recognitionProviders(config)) == QStringList{u"groq"_s});

    config.providers.prepend(ProviderConfig{.id = u"openai"_s, .kind = QString(kKindOpenAiCompatible),
                                            .name = u"OpenAI"_s, .baseUrl = u"https://api.openai.com/v1"_s,
                                            .apiKey = u"sk-test"_s, .sttModel = u"whisper-1"_s});
    config.activeProvider = u"groq"_s;
    CHECK(ids(VoiceBackends::recognitionProviders(config)) == QStringList{u"groq"_s, u"openai"_s});

    config.voice.sttProvider = u"openai"_s;
    CHECK(ids(VoiceBackends::recognitionProviders(config)) == QStringList{u"openai"_s});
    config.voice.sttProvider = u"agy"_s;
    CHECK(VoiceBackends::recognitionProviders(config).isEmpty());
}

TEST_CASE("Providers that cannot do speech say why") {
    QHash<QString, VoiceProviderChoice> choices;
    for (const VoiceProviderChoice &choice : VoiceBackends::recognitionChoices(ownersConfig())) choices.insert(choice.id, choice);
    REQUIRE(choices.size() == 6);
    CHECK(choices[u"agy"_s].reason == u"takes text only"_s);
    CHECK(choices[u"opencode"_s].reason == u"no speech models"_s);
    CHECK(choices[u"gemini"_s].reason == u"Nexa's cloud voice speaks the OpenAI audio API"_s);
    CHECK(choices[u"claude"_s].reason == u"Nexa's cloud voice speaks the OpenAI audio API"_s);
    CHECK(choices[u"keyless"_s].reason == u"no API key"_s);
    CHECK(choices[u"groq"_s].usable);
    CHECK(choices[u"groq"_s].reason.isEmpty());
    CHECK_FALSE(choices[u"agy"_s].usable);
    CHECK(VoiceBackends::speechChoices(ownersConfig()).size() == 6);
}

TEST_CASE("The cloud voice is the chosen provider's, or the first one with a voice") {
    Config config = ownersConfig();
    CHECK(VoiceBackends::speechProvider(config) == nullptr);
    config.providers[3].ttsModel = u"canopylabs/orpheus-v1-english"_s;
    REQUIRE(VoiceBackends::speechProvider(config));
    CHECK(VoiceBackends::speechProvider(config)->id == u"groq"_s);
    config.voice.ttsProvider = u"opencode"_s;
    CHECK(VoiceBackends::speechProvider(config) == nullptr);
}

TEST_CASE("Recognition without any usable provider says what is missing") {
    Config config = ownersConfig();
    config.voice.sttBackend = u"cloud"_s;
    config.voice.sttProvider = u"agy"_s;
    const Result<std::unique_ptr<ISpeechRecognizer>> recognizer = VoiceBackends::createRecognizer(config, nullptr);
    REQUIRE_FALSE(recognizer.isOk());
    CHECK(recognizer.error().message.contains(u"speech recognition model"_s));
}

TEST_CASE("A model's voices, and which models hear or speak") {
    CHECK(CloudVoices::voicesFor(u"canopylabs/orpheus-v1-english"_s)
          == QStringList{u"autumn"_s, u"diana"_s, u"hannah"_s, u"austin"_s, u"daniel"_s, u"troy"_s});
    CHECK(CloudVoices::voicesFor(u"tts-1"_s).size() == 10);
    CHECK(CloudVoices::voicesFor(u"tts-1"_s).contains(u"alloy"_s));
    CHECK(CloudVoices::voicesFor(u"qwen/qwen3.8-27b"_s).isEmpty());
    CHECK(CloudVoices::isRecognitionModel(u"whisper-large-v3"_s));
    CHECK(CloudVoices::isRecognitionModel(u"gpt-4o-mini-transcribe"_s));
    CHECK_FALSE(CloudVoices::isRecognitionModel(u"qwen/qwen3.8-27b"_s));
    CHECK(CloudVoices::isSpeechModel(u"canopylabs/orpheus-v1-english"_s));
    CHECK(CloudVoices::isSpeechModel(u"tts-1-hd"_s));
    CHECK_FALSE(CloudVoices::isSpeechModel(u"whisper-large-v3"_s));
}

TEST_CASE("Without sherpa-onnx the front hears by loudness and says how to get the spotter") {
    SherpaRuntime missing(u"/nonexistent/nexa-sherpa"_s);
    VoiceConfig voice;
    voice.wakeWord = true;
    VoiceBackends::FrontBuild build = VoiceBackends::createListeningFront(voice, &missing, true);
    REQUIRE(build.front);
    CHECK(build.front->detectorName() == u"loudness"_s);
    CHECK_FALSE(build.front->hasSpotter());
    REQUIRE_FALSE(build.problems.isEmpty());
    CHECK(build.problems.join(u' ').contains(u"scripts/download-sherpa.sh"_s));
}

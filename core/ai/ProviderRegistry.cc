#include "core/ai/ProviderRegistry.h"

#include "core/ai/AnthropicProvider.h"
#include "core/ai/GeminiProvider.h"
#include "core/ai/OpenAiProvider.h"

using namespace Qt::StringLiterals;

namespace nexa {

ProviderRegistry::ProviderRegistry(HttpClient *http) : http_(http) {
    registerKind(kKindOpenAiCompatible, [](const ProviderConfig &config, HttpClient *client) {
        return std::make_unique<OpenAiProvider>(config, client);
    });
    registerKind(kKindAnthropic, [](const ProviderConfig &config, HttpClient *client) {
        return std::make_unique<AnthropicProvider>(config, client);
    });
    registerKind(kKindGemini, [](const ProviderConfig &config, HttpClient *client) {
        return std::make_unique<GeminiProvider>(config, client);
    });
}

void ProviderRegistry::registerKind(const QString &kind, Factory factory) {
    factories_.insert(kind, std::move(factory));
}

Result<std::unique_ptr<IAiProvider>> ProviderRegistry::create(const ProviderConfig &config) const {
    const auto factory = factories_.constFind(config.kind);
    if (factory == factories_.constEnd()) {
        return Error{ErrorCode::NotSupported, u"Provider type \"%1\" is not supported yet"_s.arg(config.kind)};
    }
    if (config.baseUrl.trimmed().isEmpty()) {
        return Error{ErrorCode::InvalidArgument, u"%1 has no base URL"_s.arg(config.name)};
    }
    return (*factory)(config, http_);
}

QList<ProviderPreset> ProviderRegistry::presets() {
    const QString openAi = kKindOpenAiCompatible;
    return {
        // OmniRoute's auto/* routing combos are slow; start on a fast concrete model. Speech: a Gemini
        // chat model writes down the recording (its whisper routes need paid credits); gtts/default
        // needs no account and takes the language as its voice.
        {u"omniroute"_s, openAi, u"OmniRoute"_s, u"http://localhost:20128/v1"_s, true, u"agy/claude-sonnet-4-6"_s,
         u"anyapi/google/gemini-2.5-flash-lite"_s, u"gtts/default"_s, u"en"_s},
        {u"openai"_s, openAi, u"OpenAI"_s, u"https://api.openai.com/v1"_s, true, {}, u"whisper-1"_s, u"tts-1"_s,
         u"alloy"_s},
        {u"ollama"_s, openAi, u"Ollama (local)"_s, u"http://localhost:11434/v1"_s, false},
        {u"lmstudio"_s, openAi, u"LM Studio (local)"_s, u"http://localhost:1234/v1"_s, false},
        {u"groq"_s, openAi, u"Groq"_s, u"https://api.groq.com/openai/v1"_s, true, {}, u"whisper-large-v3-turbo"_s},
        {u"openrouter"_s, openAi, u"OpenRouter"_s, u"https://openrouter.ai/api/v1"_s, true},
        {u"deepseek"_s, openAi, u"DeepSeek"_s, u"https://api.deepseek.com/v1"_s, true},
        {u"mistral"_s, openAi, u"Mistral"_s, u"https://api.mistral.ai/v1"_s, true},
        {u"xai"_s, openAi, u"xAI"_s, u"https://api.x.ai/v1"_s, true},
        {u"together"_s, openAi, u"Together AI"_s, u"https://api.together.xyz/v1"_s, true},
        {u"custom"_s, openAi, u"Custom (OpenAI-compatible)"_s, u"http://localhost:8080/v1"_s, false},
        {u"anthropic"_s, kKindAnthropic, u"Anthropic"_s, u"https://api.anthropic.com/v1"_s, true},
        {u"gemini"_s, kKindGemini, u"Google Gemini"_s, u"https://generativelanguage.googleapis.com/v1beta"_s, true,
         u"gemini-2.5-flash"_s},
    };
}

QList<ProviderPreset> ProviderRegistry::availablePresets() const {
    QList<ProviderPreset> available;
    for (const ProviderPreset &preset : presets()) {
        if (supports(preset.kind)) available.append(preset);
    }
    return available;
}

ProviderConfig ProviderRegistry::fromPreset(const ProviderPreset &preset) {
    return ProviderConfig{
        .id = preset.id,
        .kind = preset.kind,
        .name = preset.name,
        .baseUrl = preset.baseUrl,
        .defaultModel = preset.defaultModel,
        .requiresKey = preset.requiresKey,
        .sttModel = preset.sttModel,
        .ttsModel = preset.ttsModel,
        .ttsVoice = preset.ttsVoice,
    };
}

ProviderConfig ProviderRegistry::defaultProvider() {
    return fromPreset(presets().first());
}

void ProviderRegistry::upgrade(Config &config) {
    if (config.version < 2) {
        for (ProviderConfig &provider : config.providers) {
            for (const ProviderPreset &preset : presets()) {
                if (preset.id != provider.id || preset.baseUrl != provider.baseUrl) continue;
                provider.sttModel = preset.sttModel;
                provider.ttsModel = preset.ttsModel;
                provider.ttsVoice = preset.ttsVoice;
            }
        }
    }
    config.version = Config::kVersion;
}

} // namespace nexa

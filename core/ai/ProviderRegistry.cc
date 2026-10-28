#include "core/ai/ProviderRegistry.h"

#include "core/Logging.h"
#include "core/ai/AgyProvider.h"
#include "core/ai/AnthropicProvider.h"
#include "core/ai/GeminiProvider.h"
#include "core/ai/OpenAiProvider.h"
#include "core/ai/OpenCodeProvider.h"
#include "core/ai/OpenCodeServer.h"
#include "core/ai/ProgramLookup.h"
#include "core/ai/ProviderChain.h"

#include <optional>

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
    registerKind(kKindAgy, [](const ProviderConfig &config, HttpClient *) {
        return std::make_unique<AgyProvider>(config);
    });
    registerKind(kKindOpenCode, [this](const ProviderConfig &config, HttpClient *client) {
        return std::make_unique<OpenCodeProvider>(config, client, openCodeServer(config));
    });
}

ProviderRegistry::~ProviderRegistry() = default;

std::shared_ptr<OpenCodeServer> ProviderRegistry::openCodeServer(const ProviderConfig &config) {
    const QString url = config.baseUrl.trimmed();
    const QString key = url + u'|' + config.apiKey;
    std::shared_ptr<OpenCodeServer> &server = openCodeServers_[key];
    if (!server) {
        // A URL is a server the user runs; otherwise Nexa starts opencode itself.
        const QString program = url.isEmpty() ? findProgram(u"opencode"_s) : QString();
        server = std::make_shared<OpenCodeServer>(http_, program, url.isEmpty() ? QUrl() : QUrl(url), config.apiKey);
    }
    return server;
}

void ProviderRegistry::registerKind(const QString &kind, Factory factory) {
    factories_.insert(kind, std::move(factory));
}

Result<std::unique_ptr<IAiProvider>> ProviderRegistry::create(const ProviderConfig &config) const {
    const auto factory = factories_.constFind(config.kind);
    if (factory == factories_.constEnd()) {
        return Error{ErrorCode::NotSupported, u"Provider type \"%1\" is not supported yet"_s.arg(config.kind)};
    }
    if (isHttpKind(config.kind) && config.baseUrl.trimmed().isEmpty()) {
        return Error{ErrorCode::InvalidArgument, u"%1 has no base URL"_s.arg(config.name)};
    }
    return (*factory)(config, http_);
}

Result<std::unique_ptr<IAiProvider>> ProviderRegistry::createChain(const Config &config) const {
    const ProviderConfig *first = config.active();
    if (!first) return Error{ErrorCode::NotFound, u"No AI provider is configured — open Settings."_s};
    std::vector<ProviderChain::Link> links;
    std::optional<Error> firstError;
    const auto add = [&](const ProviderConfig &provider) {
        Result<std::unique_ptr<IAiProvider>> created = create(provider);
        if (!created) {
            qCInfo(lcAi) << "left out of the chain:" << provider.name << "—" << created.error().message;
            if (!firstError) firstError = created.error();
            return;
        }
        links.push_back({std::move(created).value(), provider.defaultModel});
    };
    add(*first);
    for (const ProviderConfig &provider : config.providers) {
        if (provider.id != first->id && provider.fallback) add(provider);
    }
    if (links.empty()) return *firstError;
    return std::unique_ptr<IAiProvider>(std::make_unique<ProviderChain>(std::move(links)));
}

QList<ProviderPreset> ProviderRegistry::presets() {
    const QString openAi = kKindOpenAiCompatible;
    const QString anthropic = kKindAnthropic;
    const QString console = u"https://opencode.ai/inference"_s;
    const QString go = console + u"/go"_s;
    return {
        // The owner's own programs, signed in and paid for there: no key, Nexa runs them. An empty
        // model is the program's own default.
        {u"opencode"_s, kKindOpenCode, u"opencode"_s, {}, false},
        {u"agy"_s, kKindAgy, u"agy"_s, {}, false},
        // OpenCode's hosted API with a key from its console. The model lists live beside the
        // endpoints, not under them; GPT models there speak only the Responses API, which Nexa does
        // not, and Claude models are served in Anthropic's format.
        {u"opencode-console"_s, openAi, u"OpenCode Console"_s, console + u"/openai/v1"_s, true, {}, {}, {}, {},
         console + u"/v1/models"_s},
        {u"opencode-console-claude"_s, anthropic, u"OpenCode Console (Claude)"_s, console + u"/anthropic/v1"_s, true,
         {}, {}, {}, {}, console + u"/v1/models"_s},
        {u"opencode-go"_s, openAi, u"OpenCode Go"_s, go + u"/openai/v1"_s, true, {}, {}, {}, {}, go + u"/v1/models"_s},
        {u"opencode-go-claude"_s, anthropic, u"OpenCode Go (Claude)"_s, go + u"/anthropic/v1"_s, true, {}, {}, {}, {},
         go + u"/v1/models"_s},
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
        {u"anthropic"_s, anthropic, u"Anthropic"_s, u"https://api.anthropic.com/v1"_s, true},
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
        .modelsUrl = preset.modelsUrl,
    };
}

QList<ProviderConfig> ProviderRegistry::defaultProviders(const std::function<bool(const QString &program)> &installed) {
    QList<ProviderConfig> providers;
    for (const ProviderPreset &preset : presets()) {
        if (preset.kind != kKindOpenCode && preset.kind != kKindAgy) continue;
        if (installed(preset.kind)) providers.append(fromPreset(preset));
    }
    return providers;
}

bool ProviderRegistry::isHttpKind(const QString &kind) {
    return kind == kKindOpenAiCompatible || kind == kKindAnthropic || kind == kKindGemini;
}

bool ProviderRegistry::needsModel(const ProviderConfig &config) {
    return isHttpKind(config.kind);
}

void ProviderRegistry::upgrade(Config &config, const std::function<bool(const QString &program)> &installed) {
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
    if (config.version < 3 && config.providers.isEmpty()) {
        config.providers = defaultProviders(installed);
        if (!config.providers.isEmpty()) config.activeProvider = config.providers.first().id;
    }
    config.version = Config::kVersion;
}

} // namespace nexa

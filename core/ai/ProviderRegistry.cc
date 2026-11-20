#include "core/ai/ProviderRegistry.h"

#include "core/Logging.h"
#include "core/ai/AgyProvider.h"
#include "core/ai/AgySignIn.h"
#include "core/ai/AnthropicProvider.h"
#include "core/ai/ClaudeCodeProvider.h"
#include "core/ai/ClaudeCodeSignIn.h"
#include "core/ai/CodexProvider.h"
#include "core/ai/CodexSignIn.h"
#include "core/ai/GeminiProvider.h"
#include "core/ai/OpenAiProvider.h"
#include "core/ai/OpenCodeProvider.h"
#include "core/ai/OpenCodeServer.h"
#include "core/ai/OpenCodeSignIn.h"
#include "core/ai/ProgramLookup.h"
#include "core/ai/ProviderCatalog.h"
#include "core/ai/ProviderChain.h"

#include <QSet>

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
    registerKind(kKindCodex, [](const ProviderConfig &config, HttpClient *) {
        return std::make_unique<CodexProvider>(config);
    });
    registerKind(kKindClaudeCode, [](const ProviderConfig &config, HttpClient *) {
        return std::make_unique<ClaudeCodeProvider>(config);
    });
}

ProviderRegistry::~ProviderRegistry() = default;

std::shared_ptr<OpenCodeServer> ProviderRegistry::openCodeServer(const ProviderConfig &config) const {
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

namespace {

const QString kPrograms = u"Programs"_s;

ProviderPreset fromCatalogEntry(const CatalogEntry &entry) {
    return ProviderPreset{.id = entry.id,
                          .kind = entry.kind,
                          .name = entry.name,
                          .baseUrl = entry.baseUrl,
                          .requiresKey = entry.requiresKey,
                          .modelsUrl = entry.modelsUrl,
                          .category = entry.category,
                          .website = entry.website,
                          .hint = entry.hint,
                          .freeTier = entry.freeTier};
}

QList<ProviderPreset> buildPresets() {
    const QString openAi = kKindOpenAiCompatible;
    const QString anthropic = kKindAnthropic;
    const QString console = u"https://opencode.ai/inference"_s;
    const QString go = console + u"/go"_s;
    const QString gateways = u"Gateways"_s;
    QList<ProviderPreset> list = {
        // The owner's own programs, signed in and paid for there: no key, Nexa runs them. An empty
        // model is the program's own default (Codex: its first visible model).
        {u"opencode"_s, kKindOpenCode, u"opencode"_s, {}, false, {}, {}, {}, {}, {}, kPrograms},
        {u"agy"_s, kKindAgy, u"agy"_s, {}, false, {}, {}, {}, {}, {}, kPrograms},
        {u"codex"_s, kKindCodex, u"Codex (ChatGPT)"_s, {}, false, {}, {}, {}, {}, {}, kPrograms},
        {u"claude-code"_s, kKindClaudeCode, u"Claude Code (Claude subscription)"_s, {}, false, {}, {}, {}, {}, {}, kPrograms},
        // OpenCode's hosted API with a key from its console. The model lists live beside the
        // endpoints, not under them; GPT models there speak only the Responses API, which Nexa does
        // not, and Claude models are served in Anthropic's format.
        {u"opencode-console"_s, openAi, u"OpenCode Console"_s, console + u"/openai/v1"_s, true, {}, {}, {}, {},
         console + u"/v1/models"_s, gateways},
        {u"opencode-console-claude"_s, anthropic, u"OpenCode Console (Claude)"_s, console + u"/anthropic/v1"_s, true,
         {}, {}, {}, {}, console + u"/v1/models"_s, gateways},
        {u"opencode-go"_s, openAi, u"OpenCode Go"_s, go + u"/openai/v1"_s, true, {}, {}, {}, {}, go + u"/v1/models"_s,
         gateways},
        {u"opencode-go-claude"_s, anthropic, u"OpenCode Go (Claude)"_s, go + u"/anthropic/v1"_s, true, {}, {}, {}, {},
         go + u"/v1/models"_s, gateways},
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
        {u"custom"_s, openAi, u"Custom (OpenAI-compatible)"_s, u"http://localhost:8080/v1"_s, false, {}, {}, {}, {},
         {}, u"Local"_s},
        {u"anthropic"_s, anthropic, u"Anthropic"_s, u"https://api.anthropic.com/v1"_s, true},
        {u"gemini"_s, kKindGemini, u"Google Gemini"_s, u"https://generativelanguage.googleapis.com/v1beta"_s, true,
         u"gemini-2.5-flash"_s},
    };
    // A hand-tuned entry keeps its own URLs and speech models and takes the rest from the catalogue's entry of the
    // same id; the ids it holds are then not appended again.
    QSet<QString> taken;
    for (ProviderPreset &preset : list) {
        taken.insert(preset.id);
        if (const CatalogEntry *entry = ProviderCatalog::find(preset.id)) {
            if (preset.category.isEmpty()) preset.category = entry->category;
            preset.website = entry->website;
            preset.hint = entry->hint;
            preset.freeTier = entry->freeTier;
        }
    }
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        if (!taken.contains(entry.id)) list.append(fromCatalogEntry(entry));
    }
    return list;
}

} // namespace

const QList<ProviderPreset> &ProviderRegistry::presets() {
    static const QList<ProviderPreset> all = buildPresets();
    return all;
}

QStringList ProviderRegistry::presetCategories() {
    return QStringList{kPrograms} + ProviderCatalog::categories();
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

ProviderPreset ProviderRegistry::nexaAiPreset() {
    return {
        .id = u"nexa-ai"_s,
        .kind = QString(kKindOpenAiCompatible),
        .name = u"Nexa AI"_s,
        .baseUrl = u"https://nexadownloadmanager.com/api/assistant/v1"_s,
        .requiresKey = false,
        .defaultModel = u"nexa-fast"_s,
        .category = u"Cloud"_s,
        .website = u"https://nexadownloadmanager.com/assistant"_s,
        .hint = u"Managed Nexa AI relay (Pro & Team plans, 1,000 requests/month via your Nexa account)."_s,
        .freeTier = false,
    };
}

bool ProviderRegistry::isNexaManaged(const ProviderConfig &config) {
    return config.id == u"nexa-ai"_s || config.id.startsWith(u"nexa-ai-"_s);
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

bool ProviderRegistry::isProgramKind(const QString &kind) {
    return kind == kKindAgy || kind == kKindOpenCode || kind == kKindCodex || kind == kKindClaudeCode;
}

bool ProviderRegistry::meansNotSignedIn(const Error &error) {
    return error.code == ErrorCode::Unauthorized && error.message.contains(u"not signed in"_s, Qt::CaseInsensitive);
}

QList<PresetGroup> ProviderRegistry::groupPresets(const QList<ProviderPreset> &presets, const QStringList &categories) {
    QList<PresetGroup> groups;
    for (const QString &category : categories) {
        PresetGroup group{category, {}};
        for (int i = 0; i < presets.size(); ++i) {
            if (presets.at(i).category == category) group.indices.append(i);
        }
        if (!group.indices.isEmpty()) groups.append(group);
    }
    PresetGroup other{u"Other"_s, {}};
    for (int i = 0; i < presets.size(); ++i) {
        if (!categories.contains(presets.at(i).category)) other.indices.append(i);
    }
    if (!other.indices.isEmpty()) groups.append(other);
    return groups;
}

bool ProviderRegistry::needsModel(const ProviderConfig &config) {
    return isHttpKind(config.kind);
}

std::unique_ptr<ISignIn> ProviderRegistry::createSignIn(const ProviderConfig &config) const {
    if (config.kind == kKindAgy) {
        return std::make_unique<AgySignIn>(findProgram(u"agy"_s, config.baseUrl.trimmed()));
    }
    if (config.kind == kKindOpenCode) return std::make_unique<OpenCodeSignIn>(openCodeServer(config));
    if (config.kind == kKindCodex) {
        return std::make_unique<CodexSignIn>(CodexProvider::locate(config.baseUrl.trimmed()));
    }
    if (config.kind == kKindClaudeCode) {
        return std::make_unique<ClaudeCodeSignIn>(ClaudeCodeProvider::locate(config.baseUrl.trimmed()));
    }
    return nullptr;
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
    // 20 was the default until version 4 and stopped long tasks halfway; a number the user chose stays.
    if (config.version < 4 && config.agent.maxToolRounds == 20) {
        config.agent.maxToolRounds = AgentConfig{}.maxToolRounds;
    }
    // Mission Control is the new default design; a design the user chose stays.
    if (config.version < 5 && config.ui.design == u"studio"_s) config.ui.design = UiConfig{}.design;
    config.version = Config::kVersion;
}

} // namespace nexa

#include "core/ai/ProviderRegistry.h"

#include "core/ai/AgyEvents.h"
#include "core/ai/AgySignIn.h"
#include "core/ai/CodexEvents.h"
#include "core/ai/CodexSignIn.h"
#include "core/ai/OpenCodeSignIn.h"
#include "core/ai/ProviderCatalog.h"
#include "core/net/HttpClient.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

std::optional<ProviderPreset> preset(const QString &id) {
    for (const ProviderPreset &candidate : ProviderRegistry::presets()) {
        if (candidate.id == id) return candidate;
    }
    return std::nullopt;
}

QStringList ids(const QList<ProviderConfig> &providers) {
    QStringList result;
    for (const ProviderConfig &provider : providers) result.append(provider.id);
    return result;
}

std::function<bool(const QString &)> installedOnly(const QStringList &programs) {
    return [programs](const QString &program) { return programs.contains(program); };
}

class StubProvider : public IAiProvider {
public:
    QString id() const override { return u"stub"_s; }
    QString displayName() const override { return u"Stub"_s; }
    Capabilities capabilities() const override { return {}; }
    void listModels(std::function<void(Result<QStringList>)> done) override { done(QStringList{}); }
    std::shared_ptr<ChatStream> chat(const ChatRequest &) override { return {}; }
};

} // namespace

TEST_CASE("No preset is OmniRoute") {
    for (const ProviderPreset &candidate : ProviderRegistry::presets()) {
        CHECK(candidate.id != u"omniroute"_s);
        CHECK_FALSE(candidate.baseUrl.contains(u":20128"_s));
    }
}

TEST_CASE("The opencode and agy presets need no key and no URL") {
    const std::optional<ProviderPreset> opencode = preset(u"opencode"_s);
    REQUIRE(opencode);
    CHECK(opencode->kind == kKindOpenCode);
    CHECK_FALSE(opencode->requiresKey);
    CHECK(opencode->baseUrl.isEmpty());
    CHECK(opencode->defaultModel.isEmpty());

    const std::optional<ProviderPreset> agy = preset(u"agy"_s);
    REQUIRE(agy);
    CHECK(agy->kind == kKindAgy);
    CHECK_FALSE(agy->requiresKey);
    CHECK(agy->baseUrl.isEmpty());
    CHECK(agy->defaultModel.isEmpty());
}

TEST_CASE("OpenCode's hosted API presets list models from their own URL") {
    const QString consoleModels = u"https://opencode.ai/inference/v1/models"_s;
    const QString goModels = u"https://opencode.ai/inference/go/v1/models"_s;

    const std::optional<ProviderPreset> console = preset(u"opencode-console"_s);
    REQUIRE(console);
    CHECK(console->kind == kKindOpenAiCompatible);
    CHECK(console->baseUrl == u"https://opencode.ai/inference/openai/v1"_s);
    CHECK(console->modelsUrl == consoleModels);
    CHECK(console->requiresKey);

    const std::optional<ProviderPreset> consoleClaude = preset(u"opencode-console-claude"_s);
    REQUIRE(consoleClaude);
    CHECK(consoleClaude->kind == kKindAnthropic);
    CHECK(consoleClaude->baseUrl == u"https://opencode.ai/inference/anthropic/v1"_s);
    CHECK(consoleClaude->modelsUrl == consoleModels);

    const std::optional<ProviderPreset> go = preset(u"opencode-go"_s);
    REQUIRE(go);
    CHECK(go->kind == kKindOpenAiCompatible);
    CHECK(go->baseUrl == u"https://opencode.ai/inference/go/openai/v1"_s);
    CHECK(go->modelsUrl == goModels);

    const std::optional<ProviderPreset> goClaude = preset(u"opencode-go-claude"_s);
    REQUIRE(goClaude);
    CHECK(goClaude->kind == kKindAnthropic);
    CHECK(goClaude->baseUrl == u"https://opencode.ai/inference/go/anthropic/v1"_s);
    CHECK(goClaude->modelsUrl == goModels);

    // The preset's model list travels into the provider's settings.
    CHECK(ProviderRegistry::fromPreset(*go).modelsUrl == goModels);
}

TEST_CASE("defaultProviders offers the installed pipelines, opencode first") {
    CHECK(ids(ProviderRegistry::defaultProviders(installedOnly({u"agy"_s, u"opencode"_s})))
          == QStringList{u"opencode"_s, u"agy"_s});
    CHECK(ids(ProviderRegistry::defaultProviders(installedOnly({u"agy"_s}))) == QStringList{u"agy"_s});
    CHECK(ProviderRegistry::defaultProviders(installedOnly({})).isEmpty());
}

TEST_CASE("Upgrading a version 2 config adds the pipelines to an empty list, once") {
    Config config;
    config.version = 2;
    ProviderRegistry::upgrade(config, installedOnly({u"opencode"_s, u"agy"_s}));
    CHECK(config.version == Config::kVersion);
    CHECK(ids(config.providers) == QStringList{u"opencode"_s, u"agy"_s});
    CHECK(config.activeProvider == u"opencode"_s);

    config.providers.clear();
    ProviderRegistry::upgrade(config, installedOnly({u"opencode"_s, u"agy"_s}));
    CHECK(config.providers.isEmpty());
}

TEST_CASE("Upgrading a version 3 config lifts the old cap of 20 tool rounds and keeps a number the user chose") {
    Config config;
    config.version = 3;
    config.agent.maxToolRounds = 20;
    ProviderRegistry::upgrade(config, installedOnly({}));
    CHECK(config.agent.maxToolRounds == 200);

    Config chosen;
    chosen.version = 3;
    chosen.agent.maxToolRounds = 35;
    ProviderRegistry::upgrade(chosen, installedOnly({}));
    CHECK(chosen.agent.maxToolRounds == 35);

    Config current;
    current.agent.maxToolRounds = 20;
    ProviderRegistry::upgrade(current, installedOnly({}));
    CHECK(current.agent.maxToolRounds == 20);
}

TEST_CASE("Upgrading keeps an existing omniroute entry as it is") {
    Config config;
    config.version = 2;
    const ProviderConfig gateway{.id = u"omniroute"_s, .kind = u"openai-compatible"_s, .name = u"OmniRoute"_s,
                                 .baseUrl = u"http://localhost:20128/v1"_s, .apiKey = u"sk-kept"_s};
    config.providers = {gateway};
    config.activeProvider = gateway.id;
    ProviderRegistry::upgrade(config, installedOnly({u"opencode"_s, u"agy"_s}));
    REQUIRE(config.providers.size() == 1);
    CHECK(config.providers.first() == gateway);
}

TEST_CASE("create asks for a base URL only for HTTP kinds") {
    HttpClient http;
    ProviderRegistry registry(&http);
    registry.registerKind(kKindAgy, [](const ProviderConfig &, HttpClient *) {
        return std::make_unique<StubProvider>();
    });
    CHECK(registry.create(ProviderConfig{.id = u"agy"_s, .kind = kKindAgy}).isOk());

    const Result<std::unique_ptr<IAiProvider>> http_ =
        registry.create(ProviderConfig{.id = u"x"_s, .kind = kKindOpenAiCompatible, .name = u"X"_s});
    REQUIRE_FALSE(http_.isOk());
    CHECK(http_.error().code == ErrorCode::InvalidArgument);

    CHECK(ProviderRegistry::isHttpKind(kKindAnthropic));
    CHECK_FALSE(ProviderRegistry::isHttpKind(kKindOpenCode));
}

TEST_CASE("An API needs a model named; agy and opencode answer with their own default") {
    CHECK(ProviderRegistry::needsModel(ProviderConfig{.kind = kKindOpenAiCompatible}));
    CHECK(ProviderRegistry::needsModel(ProviderConfig{.kind = kKindAnthropic}));
    CHECK_FALSE(ProviderRegistry::needsModel(ProviderConfig{.kind = kKindAgy}));
    CHECK_FALSE(ProviderRegistry::needsModel(ProviderConfig{.kind = kKindOpenCode}));
}

TEST_CASE("The Codex preset sits beside agy: a program, no key, no URL, the first visible model") {
    const std::optional<ProviderPreset> codex = preset(u"codex"_s);
    REQUIRE(codex);
    CHECK(codex->kind == kKindCodex);
    CHECK(codex->name == u"Codex (ChatGPT)"_s);
    CHECK_FALSE(codex->requiresKey);
    CHECK(codex->baseUrl.isEmpty());
    CHECK(codex->defaultModel.isEmpty());
    CHECK(codex->category == u"Programs"_s);

    CHECK(ProviderRegistry::fromPreset(*codex).kind == kKindCodex);
    CHECK_FALSE(ProviderRegistry::isHttpKind(kKindCodex));
    CHECK_FALSE(ProviderRegistry::needsModel(ProviderConfig{.kind = kKindCodex}));

    HttpClient http;
    ProviderRegistry registry(&http);
    CHECK(registry.supports(kKindCodex));
    CHECK(registry.create(ProviderConfig{.id = u"codex"_s, .kind = kKindCodex, .name = u"Codex"_s}).isOk());
}

TEST_CASE("Codex is added by the user: it is not a default provider") {
    CHECK(ids(ProviderRegistry::defaultProviders(installedOnly({u"codex"_s, u"agy"_s, u"opencode"_s})))
          == QStringList{u"opencode"_s, u"agy"_s});
    CHECK(ProviderRegistry::defaultProviders(installedOnly({u"codex"_s})).isEmpty());
}

TEST_CASE("isProgramKind is agy, opencode and codex") {
    CHECK(ProviderRegistry::isProgramKind(kKindAgy));
    CHECK(ProviderRegistry::isProgramKind(kKindOpenCode));
    CHECK(ProviderRegistry::isProgramKind(kKindCodex));
    CHECK_FALSE(ProviderRegistry::isProgramKind(kKindOpenAiCompatible));
    CHECK_FALSE(ProviderRegistry::isProgramKind(kKindAnthropic));
    CHECK_FALSE(ProviderRegistry::isProgramKind(kKindGemini));
    CHECK_FALSE(ProviderRegistry::isProgramKind(QString()));
}

TEST_CASE("The presets list the programs first, then the hand-tuned entries, then the catalogue") {
    const QList<ProviderPreset> all = ProviderRegistry::presets();
    REQUIRE(all.size() > 20);
    CHECK(all.at(0).id == u"opencode"_s);
    CHECK(all.at(1).id == u"agy"_s);
    CHECK(all.at(2).id == u"codex"_s);
    for (int i = 0; i < 3; ++i) CHECK(all.at(i).category == u"Programs"_s);
    for (qsizetype i = 3; i < all.size(); ++i) {
        CHECK(all.at(i).category != u"Programs"_s);
        CHECK_FALSE(ProviderRegistry::isProgramKind(all.at(i).kind));
    }
    QStringList handTuned;
    for (qsizetype i = 3; i < 3 + 15; ++i) handTuned.append(all.at(i).id);
    CHECK(handTuned == QStringList{u"opencode-console"_s, u"opencode-console-claude"_s, u"opencode-go"_s,
                                   u"opencode-go-claude"_s, u"openai"_s, u"ollama"_s, u"lmstudio"_s, u"groq"_s,
                                   u"openrouter"_s, u"deepseek"_s, u"mistral"_s, u"xai"_s, u"together"_s,
                                   u"custom"_s, u"anthropic"_s});
    CHECK(all.at(3 + 15).id == u"gemini"_s);
    // Everything after is the catalogue's.
    for (qsizetype i = 3 + 16; i < all.size(); ++i) CHECK(ProviderCatalog::find(all.at(i).id) != nullptr);
}

TEST_CASE("No preset id appears twice") {
    QStringList seen;
    for (const ProviderPreset &candidate : ProviderRegistry::presets()) {
        CHECK_FALSE(seen.contains(candidate.id));
        seen.append(candidate.id);
    }
}

TEST_CASE("Every catalogue entry is a preset; the hand-tuned one wins on a clash") {
    QStringList presetIds;
    for (const ProviderPreset &candidate : ProviderRegistry::presets()) presetIds.append(candidate.id);
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        CAPTURE(entry.id);
        CHECK(presetIds.contains(entry.id));
    }

    // The hand-tuned deepseek keeps its own URLs, whatever the catalogue says.
    const std::optional<ProviderPreset> deepseek = preset(u"deepseek"_s);
    REQUIRE(deepseek);
    CHECK(deepseek->baseUrl == u"https://api.deepseek.com/v1"_s);
    CHECK(deepseek->modelsUrl.isEmpty());
    const std::optional<ProviderPreset> openai = preset(u"openai"_s);
    REQUIRE(openai);
    CHECK(openai->sttModel == u"whisper-1"_s);
    CHECK(openai->ttsVoice == u"alloy"_s);
    const std::optional<ProviderPreset> console = preset(u"opencode-console"_s);
    REQUIRE(console);
    CHECK(console->modelsUrl == u"https://opencode.ai/inference/v1/models"_s);

    // A catalogue entry that is not hand-tuned is converted as it is.
    const CatalogEntry *aihorde = ProviderCatalog::find(u"aihorde"_s);
    REQUIRE(aihorde);
    const std::optional<ProviderPreset> converted = preset(u"aihorde"_s);
    REQUIRE(converted);
    CHECK(converted->kind == aihorde->kind);
    CHECK(converted->name == aihorde->name);
    CHECK(converted->baseUrl == aihorde->baseUrl);
    CHECK(converted->modelsUrl == aihorde->modelsUrl);
    CHECK(converted->requiresKey == aihorde->requiresKey);
    CHECK(converted->category == aihorde->category);
    CHECK(converted->website == aihorde->website);
    CHECK(converted->hint == aihorde->hint);
    CHECK(converted->freeTier == aihorde->freeTier);
    CHECK(converted->defaultModel.isEmpty());
    CHECK(converted->sttModel.isEmpty());
}

TEST_CASE("A hand-tuned preset inherits category, website, hint and free tier from its catalogue twin") {
    for (const QString &id : {u"openai"_s, u"groq"_s, u"openrouter"_s, u"deepseek"_s, u"mistral"_s, u"xai"_s,
                              u"together"_s, u"anthropic"_s, u"gemini"_s, u"ollama"_s, u"lmstudio"_s}) {
        CAPTURE(id);
        const CatalogEntry *entry = ProviderCatalog::find(id);
        REQUIRE(entry);
        const std::optional<ProviderPreset> candidate = preset(id);
        REQUIRE(candidate);
        CHECK(candidate->category == entry->category);
        CHECK(candidate->website == entry->website);
        CHECK(candidate->hint == entry->hint);
        CHECK(candidate->freeTier == entry->freeTier);
    }
    const std::optional<ProviderPreset> groq = preset(u"groq"_s);
    REQUIRE(groq);
    CHECK(groq->freeTier);
    CHECK(groq->sttModel == u"whisper-large-v3-turbo"_s);
    CHECK(groq->category == u"Model makers"_s);
}

TEST_CASE("The hand-tuned entries that are not in the catalogue have a category of their own") {
    for (const QString &id : {u"opencode-console"_s, u"opencode-console-claude"_s, u"opencode-go"_s,
                              u"opencode-go-claude"_s}) {
        CAPTURE(id);
        CHECK(ProviderCatalog::find(id) == nullptr);
        const std::optional<ProviderPreset> candidate = preset(id);
        REQUIRE(candidate);
        CHECK(candidate->category == u"Gateways"_s);
    }
    const std::optional<ProviderPreset> custom = preset(u"custom"_s);
    REQUIRE(custom);
    CHECK(custom->category == u"Local"_s);
}

TEST_CASE("Every preset has a known category, a registered kind and what its kind needs") {
    HttpClient http;
    const ProviderRegistry registry(&http);
    const QStringList categories = ProviderRegistry::presetCategories();
    CHECK(registry.availablePresets().size() == ProviderRegistry::presets().size());
    for (const ProviderPreset &candidate : ProviderRegistry::presets()) {
        CAPTURE(candidate.id);
        CHECK(registry.supports(candidate.kind));
        CHECK(categories.contains(candidate.category));
        CHECK_FALSE(candidate.name.isEmpty());
        if (ProviderRegistry::isHttpKind(candidate.kind)) CHECK_FALSE(candidate.baseUrl.isEmpty());
    }
}

TEST_CASE("The categories are Programs, then the catalogue's") {
    const QStringList categories = ProviderRegistry::presetCategories();
    REQUIRE(categories.size() == ProviderCatalog::categories().size() + 1);
    CHECK(categories.first() == u"Programs"_s);
    CHECK(categories.mid(1) == ProviderCatalog::categories());
    CHECK_FALSE(ProviderCatalog::categories().contains(u"Programs"_s));
}

TEST_CASE("availablePresets keeps only the kinds a registry has") {
    HttpClient http;
    ProviderRegistry registry(&http);
    CHECK(registry.availablePresets().size() == ProviderRegistry::presets().size());
}

TEST_CASE("createSignIn makes the right sign-in for a program and none for an API") {
    HttpClient http;
    ProviderRegistry registry(&http);

    const std::unique_ptr<ISignIn> agy = registry.createSignIn(
        ProviderConfig{.id = u"agy"_s, .kind = kKindAgy, .name = u"agy"_s, .baseUrl = u"/nonexistent/agy"_s});
    REQUIRE(agy);
    CHECK(dynamic_cast<AgySignIn *>(agy.get()) != nullptr);
    CHECK_FALSE(agy->busy());

    const std::unique_ptr<ISignIn> opencode = registry.createSignIn(
        ProviderConfig{.id = u"opencode"_s, .kind = kKindOpenCode, .name = u"opencode"_s});
    REQUIRE(opencode);
    CHECK(dynamic_cast<OpenCodeSignIn *>(opencode.get()) != nullptr);

    const std::unique_ptr<ISignIn> codex = registry.createSignIn(
        ProviderConfig{.id = u"codex"_s, .kind = kKindCodex, .name = u"Codex"_s, .baseUrl = u"/nonexistent/codex"_s});
    REQUIRE(codex);
    CHECK(dynamic_cast<CodexSignIn *>(codex.get()) != nullptr);

    for (const QString &kind : {QString(kKindOpenAiCompatible), QString(kKindAnthropic), QString(kKindGemini),
                                QString(u"nonsense"_s), QString()}) {
        CAPTURE(kind);
        CHECK(registry.createSignIn(ProviderConfig{.id = u"x"_s, .kind = kind, .name = u"X"_s}) == nullptr);
    }
}

TEST_CASE("The opencode sign-in and the opencode provider share one server") {
    HttpClient http;
    ProviderRegistry registry(&http);
    const ProviderConfig config{.id = u"opencode"_s, .kind = kKindOpenCode, .name = u"opencode"_s};
    CHECK(registry.openCodeServer(config) == registry.openCodeServer(config));
}

TEST_CASE("Only the refusals that a sign-in can mend are 'not signed in'") {
    // What the programs say when nobody is signed in: Nexa's own sentence replaces it and Settings has the button.
    CHECK(ProviderRegistry::meansNotSignedIn(AgyEvents::exitError(1, u"Error: authentication required"_s)));
    CHECK(ProviderRegistry::meansNotSignedIn(Error{ErrorCode::Unauthorized, CodexEvents::notSignedInMessage()}));
    CHECK(ProviderRegistry::meansNotSignedIn(CodexEvents::exitError(1, u"not logged in"_s)));
    CHECK(ProviderRegistry::meansNotSignedIn(
        Error{ErrorCode::Unauthorized, u"opencode is not signed in for this model — run `opencode auth login`"_s}));

    // A model opencode refuses (no plan, no licence) or a server whose password is wrong is not mended by Sign in:
    // the program's own words stay, the chat must not send the user to a button that does nothing.
    CHECK_FALSE(ProviderRegistry::meansNotSignedIn(
        Error{ErrorCode::Unauthorized, u"opencode: gpt-5 refused (HTTP 403): no plan — choose another model in the toolbar"_s}));
    CHECK_FALSE(ProviderRegistry::meansNotSignedIn(
        Error{ErrorCode::Unauthorized, u"opencode's server at http://127.0.0.1:4096 refused the password — check the server "
                                       "password in Settings → Providers"_s}));
    // Not an Unauthorized at all.
    CHECK_FALSE(ProviderRegistry::meansNotSignedIn(Error{ErrorCode::Http, u"agy is not signed in"_s}));
    CHECK_FALSE(ProviderRegistry::meansNotSignedIn(Error{}));
}

TEST_CASE("The Add menu's groups: the programs first, every preset once, strays under Other") {
    const QList<ProviderPreset> all = ProviderRegistry::presets();
    const QList<PresetGroup> groups = ProviderRegistry::groupPresets(all, ProviderRegistry::presetCategories());
    REQUIRE_FALSE(groups.isEmpty());
    CHECK(groups.first().title == u"Programs"_s);
    QList<int> seen;
    for (const PresetGroup &group : groups) {
        CAPTURE(group.title);
        CHECK_FALSE(group.indices.isEmpty());       // a category with nothing in it has no submenu
        for (const int index : group.indices) {
            CHECK(all.at(index).category == group.title);
            seen.append(index);
        }
    }
    std::sort(seen.begin(), seen.end());
    CHECK(seen.size() == all.size());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());      // none twice

    // A category nobody listed, or none at all, goes under "Other", last.
    ProviderPreset stray{.id = u"s"_s, .kind = QString(kKindOpenAiCompatible), .name = u"Stray"_s};
    stray.category = u"Brand new"_s;
    ProviderPreset bare{.id = u"b"_s, .kind = QString(kKindOpenAiCompatible), .name = u"Bare"_s};
    ProviderPreset local{.id = u"l"_s, .kind = QString(kKindOpenAiCompatible), .name = u"Local one"_s};
    local.category = u"Local"_s;
    const QList<PresetGroup> some = ProviderRegistry::groupPresets({stray, local, bare}, {u"Programs"_s, u"Local"_s});
    REQUIRE(some.size() == 2);
    CHECK(some.at(0).title == u"Local"_s);
    CHECK(some.at(0).indices == QList<int>{1});
    CHECK(some.at(1).title == u"Other"_s);
    CHECK(some.at(1).indices == QList<int>{0, 2});
}

TEST_CASE("Upgrading a version 4 config moves the old default design to Mission Control and keeps a chosen one") {
    Config old;
    old.version = 4;
    old.ui.design = u"studio"_s;
    ProviderRegistry::upgrade(old, installedOnly({}));
    CHECK(old.ui.design == u"mission"_s);
    CHECK(old.version == 5);

    Config chosen;
    chosen.version = 4;
    chosen.ui.design = u"focus"_s;
    ProviderRegistry::upgrade(chosen, installedOnly({}));
    CHECK(chosen.ui.design == u"focus"_s);

    CHECK(Config{}.ui.design == u"mission"_s);
}

#include "core/ai/ProviderRegistry.h"

#include "core/net/HttpClient.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

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

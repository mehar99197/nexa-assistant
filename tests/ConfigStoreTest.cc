#include "core/config/ConfigStore.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr QFileDevice::Permissions kGroupOrOther = QFileDevice::ReadGroup | QFileDevice::WriteGroup
    | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;

Config sampleConfig() {
    Config config;
    config.providers.append(ProviderConfig{
        .id = u"gateway"_s,
        .kind = u"openai-compatible"_s,
        .name = u"Gateway"_s,
        .baseUrl = u"https://gateway.example/v1"_s,
        .apiKey = u"sk-test"_s,
        .defaultModel = u"gpt-4o-mini"_s,
    });
    config.providers.append(ProviderConfig{
        .id = u"ollama"_s,
        .kind = u"openai-compatible"_s,
        .name = u"Ollama"_s,
        .baseUrl = u"http://localhost:11434/v1"_s,
        .requiresKey = false,
    });
    config.activeProvider = u"ollama"_s;
    config.systemPrompt = u"Be brief."_s;
    config.voice.wakeWord = true;
    config.voice.ttsBackend = u"piper"_s;
    config.voice.ttsRate = 0.25;
    config.android.serial = u"R58M12345ABC"_s;
    config.android.autoReconnect = false;
    config.remote.enabled = true;
    config.remote.port = 8443;
    config.agent.maxToolRounds = 12;
    config.browser.mode = u"extension"_s;
    config.browser.profile = u"Profile 1"_s;
    config.browser.extensionToken = u"token-123"_s;
    config.memory.learn = false;
    config.skills.disabled = {u"web-browsing"_s};
    // A section this build does not know.
    config.extra.insert(u"future"_s, QJsonObject{{u"setting"_s, 42}});
    return config;
}

} // namespace

TEST_CASE("ConfigStore returns defaults when no file exists") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const Result<Config> loaded = ConfigStore(dir.filePath(u"nexa/config.json"_s)).load();
    REQUIRE(loaded.isOk());
    CHECK(loaded.value().providers.isEmpty());
    CHECK(loaded.value().systemPrompt == defaultSystemPrompt());
    CHECK(loaded.value().active() == nullptr);
}

TEST_CASE("ConfigStore round-trips providers, prompt and unknown sections") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const ConfigStore store(dir.filePath(u"nexa/config.json"_s));
    const Config config = sampleConfig();
    REQUIRE(store.save(config).isOk());

    const Result<Config> loaded = store.load();
    REQUIRE(loaded.isOk());
    CHECK(loaded.value() == config);
    REQUIRE(loaded.value().active() != nullptr);
    CHECK(loaded.value().active()->id == u"ollama"_s);
    CHECK(loaded.value().voice.wakeWord);
    CHECK(loaded.value().voice.ttsBackend == u"piper"_s);
    CHECK(loaded.value().android.serial == u"R58M12345ABC"_s);
    CHECK_FALSE(loaded.value().android.autoReconnect);
    CHECK(loaded.value().remote.enabled);
    CHECK(loaded.value().remote.port == 8443);
    CHECK(loaded.value().agent.maxToolRounds == 12);
    CHECK(loaded.value().extra.value(u"future"_s).toObject().value(u"setting"_s).toInt() == 42);
    CHECK_FALSE(loaded.value().extra.contains(u"remote"_s));
    CHECK_FALSE(loaded.value().extra.contains(u"agent"_s));
    CHECK_FALSE(loaded.value().extra.contains(u"voice"_s));
    CHECK_FALSE(loaded.value().extra.contains(u"android"_s));
    CHECK(loaded.value().browser.profile == u"Profile 1"_s);
    CHECK(loaded.value().browser.extensionToken == u"token-123"_s);
    CHECK_FALSE(loaded.value().memory.learn);
    CHECK(loaded.value().skills.disabled == QStringList{u"web-browsing"_s});
    for (const QString &section : {u"browser"_s, u"memory"_s, u"skills"_s, u"ui"_s, u"mcp"_s}) {
        CHECK_FALSE(loaded.value().extra.contains(section));
    }
}

TEST_CASE("ConfigStore round-trips fallback and modelsUrl") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const ConfigStore store(dir.filePath(u"nexa/config.json"_s));
    Config config = sampleConfig();
    config.providers[0].fallback = false;
    config.providers[0].modelsUrl = u"https://x/models"_s;
    REQUIRE(store.save(config).isOk());

    const Result<Config> loaded = store.load();
    REQUIRE(loaded.isOk());
    CHECK_FALSE(loaded.value().providers.at(0).fallback);
    CHECK(loaded.value().providers.at(0).modelsUrl == u"https://x/models"_s);
    // A provider written before the keys existed may answer for the others.
    CHECK(ProviderConfig::fromJson(QJsonObject{{u"id"_s, u"old"_s}}).fallback);
}

TEST_CASE("The browser, memory and skills sections default when missing and refuse what they cannot use") {
    const Config defaults = Config::fromJson(QJsonObject{});
    CHECK(defaults.browser.enabled);
    CHECK(defaults.browser.mode == u"auto"_s);
    CHECK(defaults.browser.browser == u"chrome"_s);
    CHECK(defaults.browser.profile.isEmpty());
    CHECK(defaults.memory.enabled);
    CHECK(defaults.memory.learn);
    CHECK(defaults.skills.enabled);
    CHECK(defaults.skills.disabled.isEmpty());

    const Config odd = Config::fromJson(QJsonObject{
        {u"browser"_s, QJsonObject{{u"mode"_s, u"telepathy"_s}, {u"browser"_s, u"../../bin"_s}, {u"profile"_s, u"../x"_s}}},
        {u"skills"_s, QJsonObject{{u"disabled"_s, QJsonArray{u"ok-name"_s, u"Bad Name"_s, u"ok-name"_s, 7}}}},
    });
    CHECK(odd.browser.mode == u"auto"_s);
    CHECK(odd.browser.browser == u"chrome"_s);
    CHECK(odd.browser.profile.isEmpty());          // a path component: only Chrome's own names
    CHECK(odd.skills.disabled == QStringList{u"ok-name"_s});
}

TEST_CASE("A phone remembered before Nexa understood the section is still remembered") {
    // Earlier builds kept "android" as an unknown section, written back untouched.
    const Config config = Config::fromJson(QJsonObject{
        {u"android"_s, QJsonObject{{u"serial"_s, u" abc123 "_s}}},
    });
    CHECK(config.android.serial == u"abc123"_s);
    CHECK(config.android.autoReconnect);            // the default when the file does not say
    CHECK_FALSE(config.extra.contains(u"android"_s));
}

TEST_CASE("The agent allows 200 tool rounds by default and up to 1000") {
    CHECK(AgentConfig{}.maxToolRounds == 200);
    CHECK(AgentConfig::fromJson(QJsonObject{}).maxToolRounds == 200);
    CHECK(AgentConfig::fromJson(QJsonObject{{u"maxToolRounds"_s, 500}}).maxToolRounds == 500);
    CHECK(AgentConfig::fromJson(QJsonObject{{u"maxToolRounds"_s, 5000}}).maxToolRounds == 1000);
    CHECK(AgentConfig::fromJson(QJsonObject{{u"maxToolRounds"_s, 0}}).maxToolRounds == 1);
}

TEST_CASE("ConfigStore keeps the file and its directory private") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(u"nexa/config.json"_s);
    REQUIRE(ConfigStore(path).save(sampleConfig()).isOk());

    const QFileDevice::Permissions file = QFileInfo(path).permissions();
    CHECK(file.testFlag(QFileDevice::ReadOwner));
    CHECK(file.testFlag(QFileDevice::WriteOwner));
    CHECK_FALSE(file.testAnyFlags(kGroupOrOther));
    CHECK_FALSE(QFileInfo(QFileInfo(path).absolutePath()).permissions().testAnyFlags(kGroupOrOther));
}

TEST_CASE("ConfigStore reports malformed JSON, leaves the file alone and can move it aside") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(u"config.json"_s);
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("{ not json");
    }
    const ConfigStore store(path);
    const Result<Config> loaded = store.load();
    REQUIRE_FALSE(loaded.isOk());
    CHECK(loaded.error().code == ErrorCode::Parse);
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::ReadOnly));
        CHECK(file.readAll() == "{ not json");
    }

    const Result<QString> moved = store.moveAside();
    REQUIRE(moved.isOk());
    CHECK_FALSE(QFile::exists(path));
    CHECK(QFile::exists(moved.value()));
}

TEST_CASE("Config falls back to the first provider when the active id is unknown") {
    Config config = sampleConfig();
    config.activeProvider = u"missing"_s;
    REQUIRE(config.active() != nullptr);
    CHECK(config.active()->id == u"gateway"_s);
}

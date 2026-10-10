#include "core/config/ConfigStore.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QFileInfo>
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
        .id = u"omniroute"_s,
        .kind = u"openai-compatible"_s,
        .name = u"OmniRoute"_s,
        .baseUrl = u"http://localhost:20128/v1"_s,
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
    config.extra.insert(u"android"_s, QJsonObject{{u"serial"_s, u"abc123"_s}});
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
    CHECK(loaded.value().extra.value(u"android"_s).toObject().value(u"serial"_s).toString() == u"abc123"_s);
    CHECK_FALSE(loaded.value().extra.contains(u"voice"_s));
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
    CHECK(config.active()->id == u"omniroute"_s);
}

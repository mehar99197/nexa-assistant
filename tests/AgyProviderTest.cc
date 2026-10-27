// core/ai/AgyProvider + AgySession against tests/fake-agy/agy, a stand-in for Google's agy CLI.

#include "core/ai/AgyProvider.h"
#include "core/ai/ChatStream.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>

#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 10'000;

// A scratch folder, the fake's script and log in it, and the environment that points the fake there.
struct AgyRig {
    explicit AgyRig(const QJsonArray &script) {
        QFile file(dir.filePath(u"script.json"_s));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(script).toJson());
        qputenv("FAKE_AGY_SCRIPT", file.fileName().toUtf8());
        qputenv("FAKE_AGY_LOG", dir.filePath(u"log.jsonl"_s).toUtf8());
    }
    ~AgyRig() {
        qunsetenv("FAKE_AGY_SCRIPT");
        qunsetenv("FAKE_AGY_LOG");
    }

    QList<QJsonObject> log(const QString &key) const {
        QList<QJsonObject> entries;
        QFile file(dir.filePath(u"log.jsonl"_s));
        if (!file.open(QIODevice::ReadOnly)) return entries;
        for (const QByteArray &line : file.readAll().split('\n')) {
            const QJsonObject entry = QJsonDocument::fromJson(line).object();
            if (entry.contains(key)) entries.append(entry);
        }
        return entries;
    }
    QStringList turns() const {
        QStringList result;
        for (const QJsonObject &entry : log(u"turn"_s)) result.append(entry.value(u"turn"_s).toString());
        return result;
    }

    ProviderConfig config(const QString &program = QStringLiteral(NEXA_TESTS_DIR "/fake-agy/agy")) const {
        return ProviderConfig{.id = u"agy"_s, .kind = kKindAgy, .name = u"agy"_s, .baseUrl = program,
                              .defaultModel = u"fake-fast"_s, .requiresKey = false};
    }
    QString workingDir() const { return dir.filePath(u"work"_s); }

    QTemporaryDir dir;
};

struct Outcome {
    QString text;
    std::optional<FinishReason> finish;
    std::optional<Error> error;
};

ChatRequest ask(const QList<ChatMessage> &history) {
    QList<ChatMessage> messages{{.role = Role::System, .text = u"You are Nexa."_s}};
    messages += history;
    return ChatRequest{.messages = messages};
}
ChatMessage u(const QString &text) { return {.role = Role::User, .text = text}; }
ChatMessage a(const QString &text) { return {.role = Role::Assistant, .text = text}; }

Outcome run(AgyProvider &provider, const ChatRequest &request) {
    Outcome outcome;
    std::shared_ptr<ChatStream> stream = provider.chat(request);
    QObject::connect(stream.get(), &ChatStream::textDelta, [&](const QString &delta) { outcome.text += delta; });
    QObject::connect(stream.get(), &ChatStream::finished, [&](FinishReason reason) { outcome.finish = reason; });
    QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &error) { outcome.error = error; });
    REQUIRE(QTest::qWaitFor([&] { return outcome.finish || outcome.error; }, kWaitMs));
    return outcome;
}

bool alive(qint64 pid) { return ::kill(pid_t(pid), 0) == 0; }

} // namespace

TEST_CASE("AgyProvider lists the models agy prints") {
    AgyRig rig({});
    AgyProvider provider(rig.config(), rig.workingDir());
    std::optional<Result<QStringList>> models;
    provider.listModels([&](Result<QStringList> result) { models = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return models.has_value(); }, kWaitMs));
    REQUIRE(models->isOk());
    CHECK(models->value() == QStringList{u"fake-fast"_s, u"fake-smart"_s});
}

TEST_CASE("A first request starts agy with Nexa's instructions and streams the reply") {
    AgyRig rig({QJsonObject{{u"text"_s, u"Hello from agy"_s}}});
    AgyProvider provider(rig.config(), rig.workingDir());
    CHECK_FALSE(provider.capabilities().nativeTools);

    const Outcome outcome = run(provider, ask({u(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.finish == FinishReason::Stop);
    CHECK(outcome.text == u"Hello from agy"_s);

    const QList<QJsonObject> starts = rig.log(u"argv"_s);
    REQUIRE(starts.size() == 1);
    const QJsonArray argv = starts.first().value(u"argv"_s).toArray();
    CHECK(argv.contains(u"--print="_s));
    CHECK(argv.contains(u"--input-format"_s));
    CHECK(argv.contains(u"--model"_s));
    CHECK(argv.contains(u"fake-fast"_s));
    REQUIRE(rig.turns().size() == 1);
    CHECK(rig.turns().first().startsWith(u"[Instructions from Nexa"_s));
    CHECK(rig.turns().first().contains(u"You are Nexa."_s));
    CHECK(rig.turns().first().endsWith(u"hello"_s));
}

TEST_CASE("The next request reuses the warm process and sends only the new turn") {
    AgyRig rig({QJsonObject{{u"text"_s, u"one"_s}}, QJsonObject{{u"text"_s, u"two"_s}}});
    AgyProvider provider(rig.config(), rig.workingDir());
    REQUIRE(run(provider, ask({u(u"first"_s)})).text == u"one"_s);
    const Outcome second = run(provider, ask({u(u"first"_s), a(u"one"_s), u(u"second"_s)}));
    CHECK(second.text == u"two"_s);
    CHECK(rig.log(u"argv"_s).size() == 1);
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1) == u"second"_s);
}

TEST_CASE("agy reaching for its own tool gets one reminder, then the step fails with the tool's name") {
    AgyRig rig({QJsonObject{{u"deny"_s, u"RunCommand"_s}}, QJsonObject{{u"deny"_s, u"RunCommand"_s}}});
    AgyProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({u(u"list my files"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::NotSupported);
    CHECK(outcome.error->message.contains(u"RunCommand"_s));
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"Use only Nexa's tools"_s));
}

TEST_CASE("A reminded agy that then answers is a normal reply") {
    AgyRig rig({QJsonObject{{u"deny"_s, u"RunCommand"_s}}, QJsonObject{{u"text"_s, u"Done properly"_s}}});
    AgyProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({u(u"list my files"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"Done properly"_s);
}

TEST_CASE("A process that dies before answering reports why") {
    AgyRig rig({QJsonObject{{u"die"_s, u"Print mode: not authenticated, trying silent auth"_s}}});
    AgyProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({u(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Unauthorized);
    CHECK(outcome.error->message.contains(u"run agy once"_s));
}

TEST_CASE("A missing program is NotFound") {
    AgyRig rig({});
    AgyProvider provider(rig.config(u"/nonexistent/agy"_s), rig.workingDir());
    const Outcome outcome = run(provider, ask({u(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::NotFound);
    CHECK(outcome.error->message.contains(u"agy is not installed"_s));
}

TEST_CASE("Cancel kills the process and the next request starts fresh") {
    AgyRig rig({QJsonObject{{u"hang"_s, true}}, QJsonObject{{u"text"_s, u"after"_s}}});
    AgyProvider provider(rig.config(), rig.workingDir());
    std::shared_ptr<ChatStream> stream = provider.chat(ask({u(u"first"_s)}));
    REQUIRE(QTest::qWaitFor([&] { return rig.turns().size() == 1; }, kWaitMs));
    stream->cancel();
    const qint64 firstPid = qint64(rig.log(u"argv"_s).first().value(u"pid"_s).toInteger());
    CHECK(QTest::qWaitFor([&] { return !alive(firstPid); }, kWaitMs));

    const Outcome next = run(provider, ask({u(u"first"_s), a(QString()), u(u"second"_s)}));
    CHECK(next.text == u"after"_s);
    CHECK(rig.log(u"argv"_s).size() == 2);
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"[Instructions from Nexa"_s));     // a fresh session gets everything
}

TEST_CASE("agy runs in Nexa's own home, whose settings deny agy's own tools") {
    AgyRig rig({QJsonObject{{u"text"_s, u"ok"_s}}});
    AgyProvider provider(rig.config(), rig.workingDir());
    REQUIRE_FALSE(run(provider, ask({u(u"hello"_s)})).error);

    const QJsonObject start = rig.log(u"argv"_s).first();
    CHECK(start.value(u"home"_s).toString().startsWith(rig.workingDir()));
    CHECK(start.value(u"cwd"_s).toString().startsWith(rig.workingDir()));
    CHECK(start.value(u"home"_s).toString() != start.value(u"cwd"_s).toString());
    const QJsonArray deny =
        start.value(u"settings"_s).toObject().value(u"permissions"_s).toObject().value(u"deny"_s).toArray();
    for (const QString &rule : {u"read_file(*)"_s, u"write_file(*)"_s, u"command(*)"_s, u"read_url(*)"_s,
                                u"execute_url(*)"_s, u"mcp(*)"_s, u"unsandboxed(*)"_s}) {
        CHECK(deny.contains(rule));
    }
}

TEST_CASE("Nexa's agy settings keep what agy itself wrote there") {
    AgyRig rig({QJsonObject{{u"text"_s, u"ok"_s}}});
    const QString settings = rig.workingDir() + u"/home/.gemini/antigravity-cli/settings.json"_s;
    REQUIRE(QDir().mkpath(QFileInfo(settings).path()));
    QFile file(settings);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(R"json({"model":"Gemini 3.8 Flash (High)","permissions":{"deny":["read_file(*)"],"allow":["command(ls)"]}})json");
    file.close();

    AgyProvider provider(rig.config(), rig.workingDir());
    REQUIRE_FALSE(run(provider, ask({u(u"hello"_s)})).error);
    const QJsonObject written = rig.log(u"argv"_s).first().value(u"settings"_s).toObject();
    CHECK(written.value(u"model"_s).toString() == u"Gemini 3.8 Flash (High)"_s);
    // Nothing may be allowed that the deny list exists to stop, and no rule appears twice.
    CHECK(written.value(u"permissions"_s).toObject().value(u"allow"_s).toArray().isEmpty());
    const QJsonArray deny = written.value(u"permissions"_s).toObject().value(u"deny"_s).toArray();
    CHECK(deny.count() == 7);
}

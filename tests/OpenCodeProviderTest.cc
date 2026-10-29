// core/ai/OpenCodeServer + OpenCodeProvider against tests/fake-opencode/opencode, a stand-in for
// `opencode serve`.

#include "core/ai/ChatStream.h"
#include "core/ai/OpenCodeProvider.h"
#include "core/ai/OpenCodeServer.h"
#include "core/ai/ProviderChain.h"
#include "core/net/HttpClient.h"

#include "QtStringMakers.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>

#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 15'000;
const QString kFake = QStringLiteral(NEXA_TESTS_DIR "/fake-opencode/opencode");

struct OpenCodeRig {
    explicit OpenCodeRig(const QJsonArray &script = {}) {
        QFile file(dir.filePath(u"script.json"_s));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(script).toJson());
        qputenv("FAKE_OPENCODE_SCRIPT", file.fileName().toUtf8());
        qputenv("FAKE_OPENCODE_LOG", dir.filePath(u"log.jsonl"_s).toUtf8());
    }
    ~OpenCodeRig() {
        qunsetenv("FAKE_OPENCODE_SCRIPT");
        qunsetenv("FAKE_OPENCODE_LOG");
    }

    QList<QJsonObject> log() const {
        QList<QJsonObject> entries;
        QFile file(dir.filePath(u"log.jsonl"_s));
        if (!file.open(QIODevice::ReadOnly)) return entries;
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (!line.isEmpty()) entries.append(QJsonDocument::fromJson(line).object());
        }
        return entries;
    }
    QList<QJsonObject> requests(const QString &method, const QString &path) const {
        QList<QJsonObject> found;
        for (const QJsonObject &entry : log()) {
            if (entry.value(u"method"_s).toString() == method && entry.value(u"path"_s).toString().contains(path)) {
                found.append(entry);
            }
        }
        return found;
    }
    std::shared_ptr<OpenCodeServer> server(const QString &program = kFake) {
        return std::make_shared<OpenCodeServer>(&http, program, QUrl(), QString(), dir.filePath(u"work"_s));
    }

    QTemporaryDir dir;
    HttpClient http;
};

ProviderConfig config() {
    return ProviderConfig{.id = u"opencode"_s, .kind = kKindOpenCode, .name = u"opencode"_s,
                          .defaultModel = u"fake/fast"_s, .requiresKey = false};
}

ChatMessage u(const QString &text) { return {.role = Role::User, .text = text}; }
ChatMessage a(const QString &text) { return {.role = Role::Assistant, .text = text}; }
ChatRequest ask(const QList<ChatMessage> &history) {
    QList<ChatMessage> messages{{.role = Role::System, .text = u"You are Nexa."_s}};
    return ChatRequest{.messages = messages + history};
}

struct Outcome {
    QString text;
    std::optional<FinishReason> finish;
    std::optional<Error> error;
};

Outcome run(OpenCodeProvider &provider, const ChatRequest &request) {
    Outcome outcome;
    std::shared_ptr<ChatStream> stream = provider.chat(request);
    QObject::connect(stream.get(), &ChatStream::textDelta, [&](const QString &delta) { outcome.text += delta; });
    QObject::connect(stream.get(), &ChatStream::finished, [&](FinishReason reason) { outcome.finish = reason; });
    QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &error) { outcome.error = error; });
    REQUIRE(QTest::qWaitFor([&] { return outcome.finish || outcome.error; }, kWaitMs));
    return outcome;
}

std::optional<Result<QStringList>> models(OpenCodeProvider &provider) {
    std::optional<Result<QStringList>> result;
    provider.listModels([&](Result<QStringList> listed) { result = std::move(listed); });
    REQUIRE(QTest::qWaitFor([&] { return result.has_value(); }, kWaitMs));
    return result;
}

QString promptText(const QJsonObject &request) {
    return request.value(u"body"_s).toObject().value(u"parts"_s).toArray().at(0).toObject().value(u"text"_s).toString();
}

bool alive(qint64 pid) { return ::kill(pid_t(pid), 0) == 0; }

} // namespace

TEST_CASE("Nexa starts its own opencode server with a password and lists its models, the default first") {
    OpenCodeRig rig;
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    CHECK_FALSE(provider.capabilities().nativeTools);
    const std::optional<Result<QStringList>> listed = models(provider);
    REQUIRE(listed->isOk());
    CHECK(listed->value().first() == u"fake/smart"_s);
    CHECK(listed->value().contains(u"other/one"_s));

    // Started as `serve` on loopback, port chosen by itself, with opencode's own tools denied.
    const QJsonObject start = rig.log().first();
    const QJsonArray argv = start.value(u"argv"_s).toArray();
    CHECK(argv.contains(u"serve"_s));
    CHECK(argv.contains(u"127.0.0.1"_s));
    CHECK(start.value(u"cwd"_s).toString().startsWith(rig.dir.filePath(u"work"_s)));
    const QJsonObject permission =
        QJsonDocument::fromJson(start.value(u"config"_s).toString().toUtf8()).object().value(u"permission"_s).toObject();
    CHECK(permission.value(u"bash"_s).toString() == u"deny"_s);
    CHECK(permission.value(u"edit"_s).toString() == u"deny"_s);
}

TEST_CASE("A request streams the reply with Nexa's system prompt and opencode's tools off") {
    OpenCodeRig rig({QJsonObject{{u"text"_s, u"Hello from opencode"_s}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    const Outcome outcome = run(provider, ask({u(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.finish == FinishReason::Stop);
    CHECK(outcome.text == u"Hello from opencode"_s);

    const QList<QJsonObject> prompts = rig.requests(u"POST"_s, u"/prompt_async"_s);
    REQUIRE(prompts.size() == 1);
    const QJsonObject body = prompts.first().value(u"body"_s).toObject();
    CHECK(body.value(u"system"_s).toString() == u"You are Nexa."_s);
    CHECK(body.value(u"tools"_s).toObject() == QJsonObject{{u"*"_s, false}});
    CHECK(body.value(u"model"_s).toObject()
          == QJsonObject{{u"providerID"_s, u"fake"_s}, {u"modelID"_s, u"fast"_s}});
    CHECK(promptText(prompts.first()) == u"hello"_s);
}

TEST_CASE("The model's reasoning never reaches the reply") {
    OpenCodeRig rig({QJsonObject{{u"reasoning"_s, u"The user wants two things. Let's plan."_s},
                                 {u"text"_s, u"Volume is 12%."_s}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    const Outcome outcome = run(provider, ask({u(u"volume?"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"Volume is 12%."_s);
}

TEST_CASE("The next request continues the same session and sends only the new turn") {
    OpenCodeRig rig({QJsonObject{{u"text"_s, u"one"_s}}, QJsonObject{{u"text"_s, u"two"_s}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    REQUIRE(run(provider, ask({u(u"first"_s)})).text == u"one"_s);
    CHECK(run(provider, ask({u(u"first"_s), a(u"one"_s), u(u"second"_s)})).text == u"two"_s);
    CHECK(rig.requests(u"POST"_s, u"/session"_s).size() == 3);      // one session, two prompts
    const QList<QJsonObject> prompts = rig.requests(u"POST"_s, u"/prompt_async"_s);
    REQUIRE(prompts.size() == 2);
    CHECK(promptText(prompts.at(1)) == u"second"_s);
}

TEST_CASE("A quota retry gives up at once and says so") {
    OpenCodeRig rig({QJsonObject{{u"retry"_s, u"Free usage exceeded, subscribe to Go"_s},
                                 {u"reason"_s, u"free_tier_limit"_s}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    const Outcome outcome = run(provider, ask({u(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Http);
    CHECK(outcome.error->message.startsWith(u"HTTP 429: opencode:"_s));
    CHECK(outcome.error->message.contains(u"fake/fast"_s));                 // which model ran out
    CHECK(outcome.error->message.contains(u"choose another model"_s));     // and what to do
    CHECK(QTest::qWaitFor([&] { return !rig.requests(u"POST"_s, u"/session/ses_1/abort"_s).isEmpty(); }, kWaitMs));
}

TEST_CASE("An auth error tells the user to sign in") {
    OpenCodeRig rig({QJsonObject{{u"autherror"_s, u"token expired"_s}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    const Outcome outcome = run(provider, ask({u(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Unauthorized);
    CHECK(outcome.error->message.contains(u"opencode auth login"_s));
}

TEST_CASE("Cancel aborts the session") {
    OpenCodeRig rig({QJsonObject{{u"hang"_s, true}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    std::shared_ptr<ChatStream> stream = provider.chat(ask({u(u"hello"_s)}));
    REQUIRE(QTest::qWaitFor([&] { return !rig.requests(u"POST"_s, u"/prompt_async"_s).isEmpty(); }, kWaitMs));
    stream->cancel();
    CHECK(QTest::qWaitFor([&] { return !rig.requests(u"POST"_s, u"/session/ses_1/abort"_s).isEmpty(); }, kWaitMs));
}

TEST_CASE("A server that never says it is listening fails within the limit") {
    OpenCodeRig rig;
    std::shared_ptr<OpenCodeServer> server = rig.server(QStringLiteral(NEXA_TESTS_DIR "/fake-opencode/silent"));
    server->setStartTimeoutMs(1'500);
    std::optional<Result<void>> started;
    server->ensureStarted([&](Result<void> result) { started = result; });
    REQUIRE(QTest::qWaitFor([&] { return started.has_value(); }, kWaitMs));
    REQUIRE_FALSE(started->isOk());
    CHECK(started->error().code == ErrorCode::Timeout);
}

TEST_CASE("A missing opencode is NotFound") {
    OpenCodeRig rig;
    OpenCodeProvider provider(config(), &rig.http, rig.server(QString()));
    const std::optional<Result<QStringList>> listed = models(provider);
    REQUIRE_FALSE(listed->isOk());
    CHECK(listed->error().code == ErrorCode::NotFound);
    CHECK(listed->error().message.contains(u"opencode is not installed"_s));
}

TEST_CASE("Attach mode uses the given URL and password and starts nothing") {
    OpenCodeRig rig;
    QProcess own;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"OPENCODE_SERVER_PASSWORD"_s, u"pw"_s);
    own.setProcessEnvironment(environment);
    own.start(kFake, {u"serve"_s, u"--hostname"_s, u"127.0.0.1"_s, u"--port"_s, u"0"_s});
    REQUIRE(own.waitForStarted());
    QByteArray line;
    REQUIRE(QTest::qWaitFor([&] {
        line += own.readAllStandardOutput();
        return line.contains("listening on");
    }, kWaitMs));
    const QString url = QString::fromUtf8(line).section(u"listening on "_s, 1).trimmed();

    auto server = std::make_shared<OpenCodeServer>(&rig.http, QString(), QUrl(url), u"pw"_s, rig.dir.filePath(u"work"_s));
    OpenCodeProvider provider(config(), &rig.http, server);
    const std::optional<Result<QStringList>> listed = models(provider);
    REQUIRE(listed->isOk());
    CHECK(server->processId() == 0);
    own.kill();
    own.waitForFinished();
}

TEST_CASE("Destroying the server stops the process") {
    OpenCodeRig rig;
    std::shared_ptr<OpenCodeServer> server = rig.server();
    std::optional<Result<void>> started;
    server->ensureStarted([&](Result<void> result) { started = result; });
    REQUIRE(QTest::qWaitFor([&] { return started.has_value(); }, kWaitMs));
    REQUIRE(started->isOk());
    const qint64 pid = server->processId();
    REQUIRE(pid > 0);
    server.reset();
    CHECK(QTest::qWaitFor([&] { return !alive(pid); }, kWaitMs));
}

TEST_CASE("An attached server that is not running fails at once, not at the start limit") {
    OpenCodeRig rig;
    auto server = std::make_shared<OpenCodeServer>(&rig.http, QString(), QUrl(u"http://127.0.0.1:9"_s), u"pw"_s,
                                                   rig.dir.filePath(u"work"_s));
    std::optional<Result<void>> started;
    QElapsedTimer clock;
    clock.start();
    server->ensureStarted([&](Result<void> result) { started = result; });
    REQUIRE(QTest::qWaitFor([&] { return started.has_value(); }, kWaitMs));
    REQUIRE_FALSE(started->isOk());
    CHECK(started->error().code == ErrorCode::ConnectionRefused);
    CHECK(clock.elapsed() < 3'000);
}

TEST_CASE("A late idle from the last turn does not end the next one before it is sent") {
    // opencode ends a turn with session.status idle and then session.idle; the second can arrive
    // after the next turn has started and before its prompt went out.
    OpenCodeRig rig({QJsonObject{{u"text"_s, u"one"_s}}, QJsonObject{{u"text"_s, u"two"_s}}});
    std::shared_ptr<OpenCodeServer> server = rig.server();
    OpenCodeProvider provider(config(), &rig.http, server);
    REQUIRE(run(provider, ask({u(u"first"_s)})).text == u"one"_s);

    Outcome outcome;
    std::shared_ptr<ChatStream> stream = provider.chat(ask({u(u"first"_s), a(u"one"_s), u(u"second"_s)}));
    QObject::connect(stream.get(), &ChatStream::textDelta, [&](const QString &delta) { outcome.text += delta; });
    QObject::connect(stream.get(), &ChatStream::finished, [&](FinishReason reason) { outcome.finish = reason; });
    QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &error) { outcome.error = error; });
    OpenCodeEvent stale;
    stale.kind = OpenCodeEvent::Kind::Idle;
    stale.sessionId = u"ses_1"_s;
    emit server->event(stale);
    REQUIRE(QTest::qWaitFor([&] { return outcome.finish || outcome.error; }, kWaitMs));
    CHECK(outcome.text == u"two"_s);
    CHECK(rig.requests(u"POST"_s, u"/prompt_async"_s).size() == 2);
}

TEST_CASE("Nexa's opencode server dies with Nexa, even when Nexa crashes") {
    OpenCodeRig rig;
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    REQUIRE(models(provider)->isOk());
    CHECK(rig.log().first().value(u"pdeathsig"_s).toInt() == SIGKILL);
}

TEST_CASE("opencode saying nothing for too long fails the turn and stops the session") {
    OpenCodeRig rig({QJsonObject{{u"hang"_s, true}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    provider.setTurnTimeoutMs(700);
    const Outcome outcome = run(provider, ask({u(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Timeout);
    CHECK(QTest::qWaitFor([&] { return !rig.requests(u"POST"_s, u"/session/ses_1/abort"_s).isEmpty(); }, kWaitMs));
}

TEST_CASE("An attached server that refuses the password says so at once") {
    OpenCodeRig rig;
    QProcess own;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"OPENCODE_SERVER_PASSWORD"_s, u"pw"_s);
    own.setProcessEnvironment(environment);
    own.start(kFake, {u"serve"_s, u"--hostname"_s, u"127.0.0.1"_s, u"--port"_s, u"0"_s});
    REQUIRE(own.waitForStarted());
    QByteArray line;
    REQUIRE(QTest::qWaitFor([&] {
        line += own.readAllStandardOutput();
        return line.contains("listening on");
    }, kWaitMs));
    const QString url = QString::fromUtf8(line).section(u"listening on "_s, 1).trimmed();

    auto server = std::make_shared<OpenCodeServer>(&rig.http, QString(), QUrl(url), u"wrong"_s,
                                                   rig.dir.filePath(u"work"_s));
    std::optional<Result<void>> started;
    QElapsedTimer clock;
    clock.start();
    server->ensureStarted([&](Result<void> result) { started = result; });
    REQUIRE(QTest::qWaitFor([&] { return started.has_value(); }, kWaitMs));
    REQUIRE_FALSE(started->isOk());
    CHECK(started->error().code == ErrorCode::Unauthorized);
    CHECK(started->error().message.contains(u"password"_s));
    CHECK(clock.elapsed() < 3'000);
    own.kill();
    own.waitForFinished();
}

TEST_CASE("opencode says which of its models take images, once it has listed them") {
    OpenCodeRig rig;
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    CHECK_FALSE(provider.takesImages(u"fake/fast"_s).has_value());     // not listed yet: unknown
    REQUIRE(models(provider)->isOk());
    CHECK(provider.takesImages(u"fake/fast"_s) == std::optional<bool>(true));
    CHECK(provider.takesImages(u"fake/smart"_s) == std::optional<bool>(false));
    CHECK_FALSE(provider.takesImages(u"other/one"_s).has_value());
    CHECK(provider.imageModels() == QStringList{u"fake/fast"_s});
}

TEST_CASE("A retired model fails at once with what to do, instead of repeating its notice") {
    // What the Antigravity plugin answered for Gemini 3 Pro on 2026-10-28, over and over for 140 s.
    OpenCodeRig rig({QJsonObject{{u"text"_s, u"Gemini 3 Pro is no longer available. Please switch to Gemini 3.1 Pro "
                                              "in the latest version of Antigravity."_s}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    const Outcome outcome = run(provider, ask({u(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::NotFound);       // the chain may ask another provider
    CHECK(outcome.error->message.contains(u"fake/fast"_s));
    CHECK(outcome.error->message.contains(u"no longer available"_s));
    CHECK(outcome.error->message.contains(u"choose another model"_s));
    CHECK(outcome.text.isEmpty());                            // nothing of the notice reached the chat
    CHECK(QTest::qWaitFor([&] { return !rig.requests(u"POST"_s, u"/abort"_s).isEmpty(); }, kWaitMs));
}

TEST_CASE("A short reply is still delivered whole, held back or not") {
    OpenCodeRig rig({QJsonObject{{u"text"_s, u"Yes."_s}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    CHECK(run(provider, ask({u(u"ok?"_s)})).text == u"Yes."_s);
}

TEST_CASE("A provider out of credits or without the plan counts as unable to answer, with the model named") {
    OpenCodeRig rig({QJsonObject{{u"apierror"_s, u"You're out of credits. Add more at https://hyper.charm.land"_s},
                                 {u"status"_s, 402}},
                     QJsonObject{{u"apierror"_s, u"Error 403: the user is not subscribed to required model plan"_s},
                                 {u"status"_s, 403}}});
    OpenCodeProvider provider(config(), &rig.http, rig.server());
    const Outcome credits = run(provider, ask({u(u"hello"_s)}));
    REQUIRE(credits.error);
    CHECK(credits.error->message.startsWith(u"HTTP 402: opencode: fake/fast"_s));
    CHECK(credits.error->message.contains(u"out of credits"_s));
    CHECK(ProviderChain::meansUnavailable(*credits.error));

    const Outcome plan = run(provider, ask({u(u"hello again"_s)}));
    REQUIRE(plan.error);
    CHECK(plan.error->code == ErrorCode::Unauthorized);
    CHECK(plan.error->message.contains(u"fake/fast"_s));
    CHECK(plan.error->message.contains(u"not subscribed"_s));
}

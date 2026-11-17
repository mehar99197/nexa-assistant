// core/ai/ClaudeCodeProvider + ClaudeCodeSession against tests/fake-claude/claude, a stand-in for Claude Code's
// print mode. No real Claude Code is ever started here.

#include "core/ai/ClaudeCodeProvider.h"
#include "core/ai/ChatStream.h"
#include "core/ai/ClaudeCodeSession.h"
#include "core/ai/ProviderChain.h"
#include "core/ai/ProviderRegistry.h"
#include "core/ProcessUtils.h"

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

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 10'000;
#ifdef Q_OS_WIN
const QString kFake = QStringLiteral(NEXA_TESTS_DIR "/fake-claude/claude.cmd");
#else
const QString kFake = QStringLiteral(NEXA_TESTS_DIR "/fake-claude/claude");
#endif

// A scratch folder, the fake's script and log in it, and the environment that points the fake there. The two variables
// that mark a Claude Code terminal are set, so the tests see them removed.
struct ClaudeRig {
    explicit ClaudeRig(const QJsonArray &script) {
        QFile file(dir.filePath(u"script.json"_s));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(script).toJson());
        qputenv("FAKE_CLAUDE_SCRIPT", file.fileName().toUtf8());
        qputenv("FAKE_CLAUDE_LOG", dir.filePath(u"log.jsonl"_s).toUtf8());
        qputenv("CLAUDECODE", "1");
        qputenv("CLAUDE_CODE_ENTRYPOINT", "cli");
    }
    ~ClaudeRig() {
        for (const char *name : {"FAKE_CLAUDE_SCRIPT", "FAKE_CLAUDE_LOG", "CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT"}) {
            qunsetenv(name);
        }
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
    QJsonArray argv(int start = 0) const { return log(u"argv"_s).at(start).value(u"argv"_s).toArray(); }
    qint64 pid(int start = 0) const {
        const QList<QJsonObject> starts = log(u"argv"_s);
        return start < starts.size() ? qint64(starts.at(start).value(u"pid"_s).toInteger()) : 0;
    }

    ProviderConfig config(const QString &program = kFake, const QString &model = u"haiku"_s) const {
        return ProviderConfig{.id = u"claude-code"_s, .kind = kKindClaudeCode, .name = u"Claude Code"_s,
                              .baseUrl = program, .defaultModel = model, .requiresKey = false};
    }
    QString workingDir() const { return dir.filePath(u"claude"_s); }

    QTemporaryDir dir;
};

struct Outcome {
    QString text;
    std::optional<FinishReason> finish;
    std::optional<Error> error;
};

ChatRequest ask(const QList<ChatMessage> &history, const QString &system = u"You are Nexa."_s) {
    QList<ChatMessage> messages{{.role = Role::System, .text = system}};
    messages += history;
    return ChatRequest{.messages = messages};
}
ChatMessage user(const QString &text) { return {.role = Role::User, .text = text}; }
ChatMessage assistant(const QString &text) { return {.role = Role::Assistant, .text = text}; }

Outcome run(ClaudeCodeProvider &provider, const ChatRequest &request) {
    Outcome outcome;
    std::shared_ptr<ChatStream> stream = provider.chat(request);
    QObject::connect(stream.get(), &ChatStream::textDelta, [&](const QString &delta) { outcome.text += delta; });
    QObject::connect(stream.get(), &ChatStream::finished, [&](FinishReason reason) { outcome.finish = reason; });
    QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &error) { outcome.error = error; });
    REQUIRE(QTest::qWaitFor([&] { return outcome.finish || outcome.error; }, kWaitMs));
    return outcome;
}

bool alive(qint64 pid) { return process::isAlive(pid); }

QJsonObject entry(const char *json) { return QJsonDocument::fromJson(QByteArray(json)).object(); }

int indexOf(const QJsonArray &argv, const QString &flag) {
    for (int i = 0; i < argv.size(); ++i) {
        if (argv.at(i).toString() == flag) return i;
    }
    return -1;
}

} // namespace

TEST_CASE("ClaudeCodeProvider offers Claude Code's model aliases and sees pictures", "[claude-code]") {
    ClaudeRig rig({});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    CHECK(provider.capabilities().vision);
    CHECK(provider.capabilities().streaming);
    CHECK_FALSE(provider.capabilities().nativeTools);
    CHECK(provider.takesImages(u"haiku"_s) == std::optional<bool>(true));
    CHECK(provider.takesImages({}) == std::optional<bool>(true));
    CHECK(provider.imageModels() == QStringList{u"fable"_s, u"opus"_s, u"sonnet"_s, u"haiku"_s});

    std::optional<Result<QStringList>> models;
    provider.listModels([&](Result<QStringList> result) { models = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return models.has_value(); }, kWaitMs));
    REQUIRE(models->isOk());
    CHECK(models->value() == QStringList{u"fable"_s, u"opus"_s, u"sonnet"_s, u"haiku"_s});
    CHECK(rig.log(u"argv"_s).isEmpty());       // no program is started to say so
}

TEST_CASE("A first request starts claude with Nexa's prompt as a flag, its own tools off, and streams the reply",
          "[claude-code]") {
    ClaudeRig rig({entry(R"({"text":"Hello from Claude"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());

    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.finish == FinishReason::Stop);
    CHECK(outcome.text == u"Hello from Claude"_s);

    const QList<QJsonObject> starts = rig.log(u"argv"_s);
    REQUIRE(starts.size() == 1);
    const QJsonArray argv = rig.argv();
    for (const QString &flag : {u"-p"_s, u"--verbose"_s, u"--include-partial-messages"_s, u"--strict-mcp-config"_s,
                                u"--disable-slash-commands"_s, u"--no-session-persistence"_s}) {
        CAPTURE(flag);
        CHECK(argv.contains(flag));
    }
    CHECK(argv.at(indexOf(argv, u"--input-format"_s) + 1).toString() == u"stream-json"_s);
    CHECK(argv.at(indexOf(argv, u"--output-format"_s) + 1).toString() == u"stream-json"_s);
    CHECK(argv.at(indexOf(argv, u"--tools"_s) + 1).toString().isEmpty());
    CHECK(argv.at(indexOf(argv, u"--setting-sources"_s) + 1).toString().isEmpty());
    CHECK(argv.at(indexOf(argv, u"--permission-mode"_s) + 1).toString() == u"dontAsk"_s);
    CHECK(argv.at(indexOf(argv, u"--permission-prompts"_s) + 1).toString() == u"none"_s);
    CHECK(argv.at(indexOf(argv, u"--model"_s) + 1).toString() == u"haiku"_s);
    CHECK(argv.at(indexOf(argv, u"--system-prompt"_s) + 1).toString() == u"You are Nexa."_s);
    // Never the flags that would change the program's own sign-in, or let it act.
    CHECK_FALSE(argv.contains(u"--bare"_s));
    CHECK_FALSE(argv.contains(u"--dangerously-skip-permissions"_s));
    CHECK(starts.first().value(u"systemPrompt"_s).toString() == u"You are Nexa."_s);
    // The turn carries the words alone: the instructions went as the system prompt.
    REQUIRE(rig.turns().size() == 1);
    CHECK(rig.turns().first() == u"hello"_s);

    // In its own empty folder, in an environment without the Claude Code terminal's marks and no config folder of
    // Nexa's, with the program's folder first on the PATH; and it dies with Nexa.
    CHECK(starts.first().value(u"cwd"_s).toString().startsWith(rig.workingDir()));
    const QJsonObject env = starts.first().value(u"env"_s).toObject();
    CHECK(env.value(u"CLAUDECODE"_s).isNull());
    CHECK(env.value(u"CLAUDE_CODE_ENTRYPOINT"_s).isNull());
    CHECK(env.value(u"CLAUDE_CONFIG_DIR"_s).toString() == qEnvironmentVariable("CLAUDE_CONFIG_DIR"));
    CHECK(starts.first().value(u"pathFirst"_s).toString() == QFileInfo(kFake).absolutePath());
#ifdef Q_OS_UNIX
    CHECK(starts.first().value(u"pdeathsig"_s).toInt() == SIGKILL);
#endif
}

TEST_CASE("The next request reuses the warm process and sends only the new turn", "[claude-code]") {
    ClaudeRig rig({entry(R"({"text":"one"})"), entry(R"({"text":"two"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    REQUIRE(run(provider, ask({user(u"first"_s)})).text == u"one"_s);
    const Outcome second = run(provider, ask({user(u"first"_s), assistant(u"one"_s), user(u"second"_s)}));
    CHECK(second.text == u"two"_s);
    CHECK(rig.log(u"argv"_s).size() == 1);
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1) == u"second"_s);
}

TEST_CASE("A changed system prompt is a new process, given the whole conversation again", "[claude-code]") {
    ClaudeRig rig({entry(R"({"text":"one"})"), entry(R"({"text":"two"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    REQUIRE(run(provider, ask({user(u"first"_s)})).text == u"one"_s);
    const Outcome second =
        run(provider, ask({user(u"first"_s), assistant(u"one"_s), user(u"second"_s)}, u"You are Nexa, brief."_s));
    CHECK(second.text == u"two"_s);
    const QList<QJsonObject> starts = rig.log(u"argv"_s);
    REQUIRE(starts.size() == 2);
    CHECK(starts.at(1).value(u"systemPrompt"_s).toString() == u"You are Nexa, brief."_s);
    CHECK_FALSE(alive(rig.pid(0)));
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"[Earlier in this conversation]"_s));
    CHECK(rig.turns().at(1).contains(u"first"_s));
    CHECK(rig.turns().at(1).endsWith(u"second"_s));
}

TEST_CASE("A changed model is a new process", "[claude-code]") {
    ClaudeRig rig({entry(R"({"text":"one"})"), entry(R"({"text":"two"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    REQUIRE(run(provider, ask({user(u"first"_s)})).text == u"one"_s);
    ChatRequest request = ask({user(u"first"_s), assistant(u"one"_s), user(u"second"_s)});
    request.model = u"sonnet"_s;
    CHECK(run(provider, request).text == u"two"_s);
    REQUIRE(rig.log(u"argv"_s).size() == 2);
    CHECK(rig.argv(1).at(indexOf(rig.argv(1), u"--model"_s) + 1).toString() == u"sonnet"_s);
}

TEST_CASE("An empty model is Claude Code's own default", "[claude-code]") {
    ClaudeRig rig({entry(R"({"text":"ok"})")});
    ClaudeCodeProvider provider(rig.config(kFake, {}), rig.workingDir());
    REQUIRE_FALSE(run(provider, ask({user(u"hello"_s)})).error);
    CHECK_FALSE(rig.argv().contains(u"--model"_s));
}

TEST_CASE("Nobody signed in: the turn fails as 'not signed in', nothing streamed, so Settings is asked and the chain moves on",
          "[claude-code]") {
    ClaudeRig rig({entry(R"({"not-signed-in":true})"), entry(R"({"text":"after"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Unauthorized);
    CHECK(ProviderRegistry::meansNotSignedIn(*outcome.error));
    CHECK(ProviderChain::meansUnavailable(*outcome.error));
    CHECK(outcome.text.isEmpty());          // "Not logged in · Please run /login" is not reply text
    // The program exited; the next request starts a new one.
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid()); }, kWaitMs));
    CHECK(run(provider, ask({user(u"hello"_s)})).text == u"after"_s);
    CHECK(rig.log(u"argv"_s).size() == 2);
}

TEST_CASE("A process that dies before answering reports why", "[claude-code]") {
    ClaudeRig rig({entry(R"({"die":"Not logged in · Please run /login"})"), entry(R"({"die":"segmentation fault"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    const Outcome signedOut = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(signedOut.error);
    CHECK(signedOut.error->code == ErrorCode::Unauthorized);
    CHECK(signedOut.error->message.contains(u"Sign in"_s));

    const Outcome crashed = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(crashed.error);
    CHECK(crashed.error->code == ErrorCode::ConnectionRefused);
    CHECK(crashed.error->message.contains(u"segmentation fault"_s));
}

TEST_CASE("A usage limit is HTTP 429, which the chain treats as 'cannot answer, ask the next'", "[claude-code]") {
    ClaudeRig rig({entry(R"({"rate-limit":true})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Http);
    CHECK(outcome.error->message.startsWith(u"HTTP 429: Claude Code: "_s));
    CHECK(ProviderChain::meansUnavailable(*outcome.error));
    CHECK(outcome.text.isEmpty());
}

TEST_CASE("A model Claude Code does not have is NotFound, with the aliases to choose from", "[claude-code]") {
    ClaudeRig rig({entry(R"({"error":"API Error: 404 {\"type\":\"error\",\"error\":{\"type\":\"not_found_error\",\"message\":\"model: claude-9\"}}","status":404})")});
    ClaudeCodeProvider provider(rig.config(kFake, u"claude-9"_s), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::NotFound);
    CHECK(outcome.error->message.contains(u"sonnet"_s));
}

TEST_CASE("Pictures reach Claude Code as base64 blocks before the words", "[claude-code]") {
    ClaudeRig rig({entry(R"({"text":"a cat"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    ChatMessage shown = user(u"what is this?"_s);
    shown.images = {ImageAttachment{QByteArray("fake-png"), u"image/png"_s}};
    const Outcome outcome = run(provider, ask({shown}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"a cat"_s);
    const QList<QJsonObject> turns = rig.log(u"turn"_s);
    REQUIRE(turns.size() == 1);
    const QJsonArray images = turns.first().value(u"images"_s).toArray();
    REQUIRE(images.size() == 1);
    CHECK(images.first().toObject().value(u"media_type"_s).toString() == u"image/png"_s);
    CHECK(images.first().toObject().value(u"bytes"_s).toInt() == 8);
    CHECK(turns.first().value(u"turn"_s).toString() == u"what is this?"_s);
}

TEST_CASE("A missing program is NotFound", "[claude-code]") {
    ClaudeRig rig({});
    ClaudeCodeProvider provider(rig.config(u"/nonexistent/claude"_s), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::NotFound);
    CHECK(outcome.error->message.contains(u"Claude Code is not installed"_s));
}

TEST_CASE("Cancel kills the process and the next request starts fresh", "[claude-code]") {
    ClaudeRig rig({entry(R"({"hang":true})"), entry(R"({"text":"after"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    std::shared_ptr<ChatStream> stream = provider.chat(ask({user(u"first"_s)}));
    REQUIRE(QTest::qWaitFor([&] { return rig.turns().size() == 1; }, kWaitMs));
    stream->cancel();
    const qint64 firstPid = rig.pid(0);
    CHECK(QTest::qWaitFor([&] { return !alive(firstPid); }, kWaitMs));

    const Outcome next = run(provider, ask({user(u"first"_s), assistant(QString()), user(u"second"_s)}));
    CHECK(next.text == u"after"_s);
    CHECK(rig.log(u"argv"_s).size() == 2);
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"[Earlier in this conversation]"_s));     // a fresh session gets everything
}

TEST_CASE("Claude Code saying nothing for too long fails the turn, so another provider can answer", "[claude-code]") {
    ClaudeRig rig({entry(R"({"hang":true})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    provider.setTurnTimeoutMs(700);
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Timeout);
    CHECK(ProviderChain::meansUnavailable(*outcome.error));
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid()); }, kWaitMs));
}

TEST_CASE("Thinking counts as a sign of life", "[claude-code]") {
    ClaudeRig rig({entry(R"({"think":true,"delay":0.3,"text":"thought through"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    provider.setTurnTimeoutMs(600);     // less than the three deltas take together, more than any gap between them
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"thought through"_s);     // the thinking itself is not reply text
}

TEST_CASE("warmUp starts Claude Code early; the first request, on its own prompt, replaces it", "[claude-code]") {
    ClaudeRig rig({entry(R"({"text":"ok"})")});
    ClaudeCodeProvider provider(rig.config(), rig.workingDir());
    provider.warmUp();
    REQUIRE(QTest::qWaitFor([&] { return rig.log(u"argv"_s).size() == 1; }, kWaitMs));
    CHECK_FALSE(rig.argv(0).contains(u"--system-prompt"_s));
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"ok"_s);
    REQUIRE(rig.log(u"argv"_s).size() == 2);
    CHECK(rig.argv(1).contains(u"--system-prompt"_s));
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid(0)); }, kWaitMs));
}

TEST_CASE("The session's command line and environment are what the design says (pure)", "[claude-code]") {
    const QStringList bare = ClaudeCodeSession::arguments({}, {});
    CHECK(bare.first() == u"-p"_s);
    CHECK_FALSE(bare.contains(u"--model"_s));
    CHECK_FALSE(bare.contains(u"--system-prompt"_s));
    CHECK(bare.contains(u"--no-session-persistence"_s));
    const QStringList full = ClaudeCodeSession::arguments(u"opus"_s, u"Be brief."_s);
    CHECK(full.mid(full.indexOf(u"--model"_s), 2) == QStringList{u"--model"_s, u"opus"_s});
    CHECK(full.mid(full.indexOf(u"--system-prompt"_s), 2) == QStringList{u"--system-prompt"_s, u"Be brief."_s});

    qputenv("CLAUDECODE", "1");
    qputenv("CLAUDE_CODE_ENTRYPOINT", "cli");
    const QProcessEnvironment environment = ClaudeCodeSession::prepare(kFake);
    qunsetenv("CLAUDECODE");
    qunsetenv("CLAUDE_CODE_ENTRYPOINT");
    CHECK_FALSE(environment.contains(u"CLAUDECODE"_s));
    CHECK_FALSE(environment.contains(u"CLAUDE_CODE_ENTRYPOINT"_s));
    CHECK(environment.value(u"PATH"_s).startsWith(QFileInfo(kFake).absolutePath() + u':'));
    // Never a config folder of Nexa's: the sign-in is the owner's own.
    CHECK(environment.value(u"CLAUDE_CONFIG_DIR"_s) == qEnvironmentVariable("CLAUDE_CONFIG_DIR"));
}

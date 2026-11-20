// core/ai/CodexProvider + CodexSession against tests/fake-codex/codex, a stand-in for `codex app-server`.
// No real Codex is ever started here, and no prompt is sent to a model.

#include "core/ai/ChatStream.h"
#include "core/ai/CodexProvider.h"
#include "core/ai/ProviderChain.h"
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

constexpr int kWaitMs = 12'000;

// A scratch folder, the fake's script, state and log in it, and the environment that points the fake there.
struct CodexRig {
    explicit CodexRig(const QJsonArray &script, const QJsonObject &stateOverrides = {}) {
        QJsonObject state{{u"signedIn"_s, true}, {u"plan"_s, u"plus"_s},
                          {u"email"_s, u"private.person@example.invalid"_s},
                          {u"mcp"_s, QJsonArray{u"alpha"_s, u"beta"_s}}};
        for (auto it = stateOverrides.begin(); it != stateOverrides.end(); ++it) state.insert(it.key(), it.value());
        writeFile(u"script.json"_s, QJsonDocument(script).toJson());
        writeFile(u"state.json"_s, QJsonDocument(state).toJson());
        qputenv("FAKE_CODEX_SCRIPT", dir.filePath(u"script.json"_s).toUtf8());
        qputenv("FAKE_CODEX_STATE", dir.filePath(u"state.json"_s).toUtf8());
        qputenv("FAKE_CODEX_LOG", dir.filePath(u"log.jsonl"_s).toUtf8());
    }
    ~CodexRig() {
        qunsetenv("FAKE_CODEX_SCRIPT");
        qunsetenv("FAKE_CODEX_STATE");
        qunsetenv("FAKE_CODEX_LOG");
    }

    void writeFile(const QString &name, const QByteArray &content) const {
        QFile file(dir.filePath(name));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(content);
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
    // The parameters of every request of that method, in order.
    QList<QJsonObject> requests(const QString &method) const {
        QList<QJsonObject> result;
        for (const QJsonObject &entry : log(u"request"_s)) {
            if (entry.value(u"request"_s).toString() == method) result.append(entry.value(u"params"_s).toObject());
        }
        return result;
    }
    qint64 pid(int index = 0) const {
        const QList<QJsonObject> starts = log(u"argv"_s);
        return index < starts.size() ? qint64(starts.at(index).value(u"pid"_s).toInteger()) : 0;
    }

#ifdef Q_OS_WIN
    static inline const QString kDefaultCodex = QStringLiteral(NEXA_TESTS_DIR "/fake-codex/codex.cmd");
#else
    static inline const QString kDefaultCodex = QStringLiteral(NEXA_TESTS_DIR "/fake-codex/codex");
#endif
    ProviderConfig config(const QString &program = kDefaultCodex,
                          const QString &model = u"fake-fast"_s) const {
        return ProviderConfig{.id = u"codex"_s, .kind = kKindCodex, .name = u"Codex"_s, .baseUrl = program,
                              .defaultModel = model, .requiresKey = false};
    }
    QString workingDir() const { return dir.filePath(u"codex"_s); }

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
ChatMessage user(const QString &text) { return {.role = Role::User, .text = text}; }
ChatMessage assistant(const QString &text) { return {.role = Role::Assistant, .text = text}; }

Outcome run(CodexProvider &provider, const ChatRequest &request) {
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

} // namespace

TEST_CASE("CodexProvider lists the visible models, after checking the account", "[codex]") {
    CodexRig rig({});
    CodexProvider provider(rig.config(), rig.workingDir());
    std::optional<Result<QStringList>> models;
    provider.listModels([&](Result<QStringList> result) { models = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return models.has_value(); }, kWaitMs));
    REQUIRE(models->isOk());
    CHECK(models->value() == QStringList{u"fake-fast"_s, u"fake-smart"_s});     // fake-hidden is hidden
    // The account first, and no thread (so none of the user's MCP servers starts).
    const QList<QJsonObject> calls = rig.log(u"request"_s);
    REQUIRE(calls.size() >= 3);
    CHECK(calls.at(1).value(u"request"_s).toString() == u"account/read"_s);
    CHECK(calls.at(2).value(u"request"_s).toString() == u"model/list"_s);
    CHECK(rig.requests(u"thread/start"_s).isEmpty());
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid()); }, kWaitMs));         // the check's server is gone

    // Cached: a second question starts nothing.
    std::optional<Result<QStringList>> again;
    provider.listModels([&](Result<QStringList> result) { again = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return again.has_value(); }, kWaitMs));
    CHECK(again->value() == models->value());
    CHECK(rig.log(u"argv"_s).size() == 1);
}

TEST_CASE("CodexProvider follows model/list's pages", "[codex]") {
    CodexRig rig({}, QJsonObject{{u"paginate"_s, true}});
    CodexProvider provider(rig.config(), rig.workingDir());
    std::optional<Result<QStringList>> models;
    provider.listModels([&](Result<QStringList> result) { models = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return models.has_value(); }, kWaitMs));
    REQUIRE(models->isOk());
    CHECK(models->value() == QStringList{u"fake-fast"_s, u"fake-smart"_s});
    CHECK(rig.requests(u"model/list"_s).size() == 2);
}

TEST_CASE("Signed out, there are no models to list: Unauthorized, not the bundled catalogue", "[codex]") {
    CodexRig rig({}, QJsonObject{{u"signedIn"_s, false}});
    CodexProvider provider(rig.config(), rig.workingDir());
    std::optional<Result<QStringList>> models;
    provider.listModels([&](Result<QStringList> result) { models = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return models.has_value(); }, kWaitMs));
    REQUIRE_FALSE(models->isOk());
    CHECK(models->error().code == ErrorCode::Unauthorized);
    CHECK(models->error().message == u"Codex is not signed in — sign in under Settings → Providers"_s);
    CHECK(rig.requests(u"model/list"_s).isEmpty());
}

TEST_CASE("CodexProvider says what it can do", "[codex]") {
    CodexRig rig({});
    CodexProvider provider(rig.config(), rig.workingDir());
    CHECK(provider.capabilities().streaming);
#ifdef Q_OS_WIN
    CHECK(provider.capabilities().nativeTools);
#else
    CHECK_FALSE(provider.capabilities().nativeTools);
#endif
    CHECK_FALSE(provider.capabilities().vision);
    CHECK(provider.takesImages(u"fake-fast"_s) == std::optional<bool>(false));
    CHECK(provider.imageModels().isEmpty());
    CHECK(provider.id() == u"codex"_s);
    CHECK(provider.displayName() == u"Codex"_s);
}

TEST_CASE("A first request starts a locked-down thread and streams the reply", "[codex]") {
    CodexRig rig({entry(R"({"text":"Hello from Codex"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.finish == FinishReason::Stop);
    CHECK(outcome.text == u"Hello from Codex"_s);

    const QList<QJsonObject> starts = rig.log(u"argv"_s);
    REQUIRE(starts.size() == 1);
    const QJsonArray argv = starts.first().value(u"argv"_s).toArray();
    CHECK(argv.first().toString() == u"app-server"_s);
    CHECK(argv.contains(u"shell_tool"_s));
    CHECK(argv.contains(u"skill_search"_s));
    CHECK(argv.contains(u"web_search=\"disabled\""_s));
    CHECK(starts.first().value(u"cwd"_s).toString().startsWith(rig.workingDir()));
#ifdef Q_OS_UNIX
    CHECK(starts.first().value(u"pdeathsig"_s).toInt() == SIGKILL);
#endif

    const QList<QJsonObject> threads = rig.requests(u"thread/start"_s);
    REQUIRE(threads.size() == 1);
    const QJsonObject thread = threads.first();
    CHECK(thread.value(u"model"_s).toString() == u"fake-fast"_s);
    CHECK(thread.value(u"ephemeral"_s).toBool());
    CHECK(thread.value(u"approvalPolicy"_s).toString() == u"never"_s);
    CHECK(thread.value(u"sandbox"_s).toString() == u"read-only"_s);
    CHECK(thread.value(u"cwd"_s).toString() == rig.workingDir() + u"/work"_s);
    CHECK_FALSE(thread.value(u"developerInstructions"_s).toString().isEmpty());
    // The user's own MCP servers are switched off for this thread, by name.
    const QJsonObject servers = thread.value(u"config"_s).toObject().value(u"mcp_servers"_s).toObject();
    CHECK(servers.keys().size() == 2);
    CHECK_FALSE(servers.value(u"alpha"_s).toObject().value(u"enabled"_s).toBool(true));
    CHECK_FALSE(servers.value(u"beta"_s).toObject().value(u"enabled"_s).toBool(true));
    // The private folder is for the owner alone.
    CHECK(QFileInfo(rig.workingDir() + u"/work"_s).isDir());
#ifdef Q_OS_UNIX
    CHECK((QFileInfo(rig.workingDir() + u"/work"_s).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                                                                       | QFileDevice::WriteOther)) == QFileDevice::Permissions());
#endif

    // The order of the conversation with the server: handshake, account, config, thread, turn.
    QStringList order;
    for (const QJsonObject &call : rig.log(u"request"_s)) order.append(call.value(u"request"_s).toString());
    CHECK(order == QStringList{u"initialize"_s, u"account/read"_s, u"config/read"_s, u"thread/start"_s,
                               u"turn/start"_s});
    CHECK_FALSE(rig.log(u"notification"_s).isEmpty());          // `initialized` was sent

    REQUIRE(rig.turns().size() == 1);
    CHECK(rig.turns().first().startsWith(u"[Instructions from Nexa"_s));
    CHECK(rig.turns().first().contains(u"You are Nexa."_s));
    CHECK(rig.turns().first().endsWith(u"hello"_s));
    const QJsonObject turn = rig.requests(u"turn/start"_s).first();
    CHECK(turn.value(u"threadId"_s).toString() == u"thread-1"_s);
    CHECK(turn.value(u"model"_s).toString() == u"fake-fast"_s);
}

TEST_CASE("Two agent messages come out as two paragraphs", "[codex]") {
    CodexRig rig({entry(R"({"texts":["Let me check.","It is 42."]})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"what is it"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"Let me check.\n\nIt is 42."_s);
}

TEST_CASE("A message that arrives whole, without deltas, is not lost", "[codex]") {
    CodexRig rig({entry(R"({"text":"All at once","noDeltas":true})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"All at once"_s);
}

TEST_CASE("The next request reuses the warm server and thread and sends only the new turn", "[codex]") {
    CodexRig rig({entry(R"({"text":"one"})"), entry(R"({"text":"two"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    REQUIRE(run(provider, ask({user(u"first"_s)})).text == u"one"_s);
    const Outcome second = run(provider, ask({user(u"first"_s), assistant(u"one"_s), user(u"second"_s)}));
    CHECK(second.text == u"two"_s);
    CHECK(rig.log(u"argv"_s).size() == 1);
    CHECK(rig.requests(u"thread/start"_s).size() == 1);
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1) == u"second"_s);
    CHECK(rig.requests(u"turn/start"_s).at(1).value(u"threadId"_s).toString() == u"thread-1"_s);
    CHECK(rig.requests(u"account/read"_s).size() == 1);         // asked once per server
}

TEST_CASE("A changed system prompt is sent as an update", "[codex]") {
    CodexRig rig({entry(R"({"text":"one"})"), entry(R"({"text":"two"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    REQUIRE_FALSE(run(provider, ask({user(u"first"_s)})).error);
    ChatRequest next = ask({user(u"first"_s), assistant(u"one"_s), user(u"second"_s)});
    next.messages.first().text = u"You are Nexa, and it is later."_s;
    REQUIRE_FALSE(run(provider, next).error);
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"[Nexa's instructions have changed"_s));
    CHECK(rig.turns().at(1).endsWith(u"second"_s));
}

TEST_CASE("An empty model is the first model Codex lists as visible", "[codex]") {
    CodexRig rig({entry(R"({"text":"hi"})")});
    CodexProvider provider(rig.config(rig.config().baseUrl, QString()), rig.workingDir());
    REQUIRE_FALSE(run(provider, ask({user(u"hello"_s)})).error);
    REQUIRE(rig.requests(u"thread/start"_s).size() == 1);
    CHECK(rig.requests(u"thread/start"_s).first().value(u"model"_s).toString() == u"fake-fast"_s);
    CHECK(rig.requests(u"turn/start"_s).first().value(u"model"_s).toString() == u"fake-fast"_s);
}

TEST_CASE("The request's own model beats the configured one", "[codex]") {
    CodexRig rig({entry(R"({"text":"hi"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    ChatRequest request = ask({user(u"hello"_s)});
    request.model = u"fake-smart"_s;
    REQUIRE_FALSE(run(provider, request).error);
    CHECK(rig.requests(u"thread/start"_s).first().value(u"model"_s).toString() == u"fake-smart"_s);
}

TEST_CASE("Codex reaching for its own tool is stopped at once, reminded once, and then answers", "[codex]") {
    CodexRig rig({entry(R"({"tool":"commandExecution"})"), entry(R"({"text":"Done properly"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"list my files"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"Done properly"_s);
    CHECK_FALSE(rig.log(u"interrupt"_s).isEmpty());             // not left to run
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"Use only Nexa's tools"_s));
    CHECK(rig.requests(u"thread/start"_s).size() == 1);         // the reminder goes on the same thread
}

TEST_CASE("Codex reaching for its own tool twice fails the step, naming the tool", "[codex]") {
    CodexRig rig({entry(R"({"tool":"mcpToolCall"})"), entry(R"({"tool":"mcpToolCall"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"list my files"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::NotSupported);
    CHECK(outcome.error->message.contains(u"mcpToolCall"_s));
    REQUIRE(rig.turns().size() == 2);
}

TEST_CASE("A usage limit is an error the provider chain moves on from", "[codex]") {
    CodexRig rig({entry(R"({"error":{"message":"You've hit your usage limit. Try again at 3pm.","info":"usageLimitExceeded"}})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Http);
    CHECK(outcome.error->message.startsWith(u"HTTP 429"_s));
    CHECK(outcome.error->message.contains(u"usage limit"_s));
    CHECK(ProviderChain::meansUnavailable(*outcome.error));
}

TEST_CASE("After a failed turn the next request starts a new thread with everything", "[codex]") {
    CodexRig rig({entry(R"({"error":{"message":"overloaded","info":"serverOverloaded"}})"), entry(R"({"text":"fine"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    REQUIRE(run(provider, ask({user(u"first"_s)})).error);
    const Outcome next = run(provider, ask({user(u"first"_s), assistant(QString()), user(u"second"_s)}));
    REQUIRE_FALSE(next.error);
    CHECK(next.text == u"fine"_s);
    CHECK(rig.log(u"argv"_s).size() == 1);                      // the server is still good
    CHECK(rig.requests(u"thread/start"_s).size() == 2);
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"[Instructions from Nexa"_s));
    CHECK(rig.requests(u"turn/start"_s).at(1).value(u"threadId"_s).toString() == u"thread-2"_s);
}

TEST_CASE("A turn that ends 'unauthorized' is Unauthorized with the way out", "[codex]") {
    CodexRig rig({entry(R"({"error":{"message":"Your access token could not be refreshed","info":"unauthorized"}})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Unauthorized);
    CHECK(outcome.error->message == u"Codex is not signed in — sign in under Settings → Providers"_s);
}

TEST_CASE("Signed out, a request fails Unauthorized before any thread is made", "[codex]") {
    CodexRig rig({entry(R"({"text":"never sent"})")}, QJsonObject{{u"signedIn"_s, false}});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Unauthorized);
    CHECK(outcome.error->message == u"Codex is not signed in — sign in under Settings → Providers"_s);
    CHECK(rig.requests(u"thread/start"_s).isEmpty());
    CHECK(rig.turns().isEmpty());
    CHECK(ProviderChain::meansUnavailable(*outcome.error));
}

TEST_CASE("Signing in after a refusal works on the very next request", "[codex]") {
    CodexRig rig({entry(R"({"text":"welcome"})")}, QJsonObject{{u"signedIn"_s, false}});
    CodexProvider provider(rig.config(), rig.workingDir());
    REQUIRE(run(provider, ask({user(u"hello"_s)})).error);
    // The owner signs in (in Nexa's sign-in, or in a terminal) ...
    QFile state(rig.dir.filePath(u"state.json"_s));
    REQUIRE(state.open(QIODevice::ReadOnly));
    QJsonObject signedIn = QJsonDocument::fromJson(state.readAll()).object();
    state.close();
    signedIn.insert(u"signedIn"_s, true);
    rig.writeFile(u"state.json"_s, QJsonDocument(signedIn).toJson());
    // ... and the next request is answered, by a server that starts afresh and so reads the new sign-in.
    const Outcome next = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(next.error);
    CHECK(next.text == u"welcome"_s);
    CHECK(rig.log(u"argv"_s).size() == 2);
}

TEST_CASE("If the user's config cannot be read, no thread starts: the MCP servers could not be switched off", "[codex]") {
    CodexRig rig({entry(R"({"text":"never sent"})")}, QJsonObject{{u"configFails"_s, true}});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(rig.requests(u"thread/start"_s).isEmpty());
    CHECK(rig.turns().isEmpty());
}

TEST_CASE("A missing program is NotFound", "[codex]") {
    CodexRig rig({});
    CodexProvider provider(rig.config(u"/nonexistent/codex"_s), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::NotFound);
    CHECK(outcome.error->message.contains(u"Codex is not installed"_s));
    std::optional<Result<QStringList>> models;
    provider.listModels([&](Result<QStringList> result) { models = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return models.has_value(); }, kWaitMs));
    CHECK(models->error().code == ErrorCode::NotFound);
}

TEST_CASE("A server that dies at the start reports why", "[codex]") {
    CodexRig rig({}, QJsonObject{{u"mode"_s, u"die"_s}});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Unauthorized);
}

TEST_CASE("A server that prints nothing Nexa understands is a parse error", "[codex]") {
    CodexRig rig({}, QJsonObject{{u"mode"_s, u"garbage"_s}});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Parse);
}

TEST_CASE("Stray lines in the middle of a turn are skipped", "[codex]") {
    CodexRig rig({entry(R"({"garbage":true,"text":"still fine"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"still fine"_s);
}

TEST_CASE("A server that crashes mid-turn fails the turn and the next request starts a new one", "[codex]") {
    CodexRig rig({entry(R"({"crash":true})"), entry(R"({"text":"after"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome crashed = run(provider, ask({user(u"first"_s)}));
    REQUIRE(crashed.error);
    CHECK(crashed.error->code == ErrorCode::ConnectionRefused);
    CHECK(crashed.error->message.contains(u"exit 3"_s));
    CHECK(crashed.error->message.contains(u"fake crashed"_s));
    CHECK(crashed.text == u"Half an ans"_s);                    // what was said before it died was shown

    const Outcome next = run(provider, ask({user(u"first"_s), assistant(QString()), user(u"second"_s)}));
    REQUIRE_FALSE(next.error);
    CHECK(next.text == u"after"_s);
    CHECK(rig.log(u"argv"_s).size() == 2);
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"[Instructions from Nexa"_s));
}

TEST_CASE("Every request the server makes is refused, so no turn waits for Nexa", "[codex]") {
    CodexRig rig({entry(R"({"serverRequest":true,"text":"ok"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"ok"_s);
    const QList<QJsonObject> answers = rig.log(u"response"_s);
    REQUIRE(answers.size() == 1);
    const QJsonObject answer = answers.first().value(u"response"_s).toObject();
    CHECK(answer.value(u"id"_s).toString() == u"srv-1"_s);
    CHECK(answer.value(u"error"_s).toObject().value(u"code"_s).toInt() == -32601);
}

TEST_CASE("Cancel interrupts the turn, and the next request starts a new thread on the same server", "[codex]") {
    CodexRig rig({entry(R"({"hang":true})"), entry(R"({"text":"after"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    std::shared_ptr<ChatStream> stream = provider.chat(ask({user(u"first"_s)}));
    REQUIRE(QTest::qWaitFor([&] { return rig.turns().size() == 1; }, kWaitMs));
    stream->cancel();
    REQUIRE(QTest::qWaitFor([&] { return !rig.log(u"interrupt"_s).isEmpty(); }, kWaitMs));
    const QJsonObject interrupt = rig.log(u"interrupt"_s).first().value(u"interrupt"_s).toObject();
    CHECK(interrupt.value(u"threadId"_s).toString() == u"thread-1"_s);
    CHECK(interrupt.value(u"turnId"_s).toString() == u"turn-1"_s);
    QTest::qWait(300);                  // Codex ends the turn

    const Outcome next = run(provider, ask({user(u"first"_s), assistant(QString()), user(u"second"_s)}));
    REQUIRE_FALSE(next.error);
    CHECK(next.text == u"after"_s);
    CHECK(rig.log(u"argv"_s).size() == 1);                      // no kill: the interrupt was enough
    CHECK(alive(rig.pid()));
    CHECK(rig.requests(u"thread/start"_s).size() == 2);         // its old thread held a half answer
    REQUIRE(rig.turns().size() == 2);
    CHECK(rig.turns().at(1).startsWith(u"[Instructions from Nexa"_s));
}

TEST_CASE("A cancelled turn that Codex will not end is killed after 3 seconds", "[codex]") {
    CodexRig rig({entry(R"({"hang":true,"ignoreInterrupt":true})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    std::shared_ptr<ChatStream> stream = provider.chat(ask({user(u"first"_s)}));
    REQUIRE(QTest::qWaitFor([&] { return rig.turns().size() == 1; }, kWaitMs));
    const qint64 pid = rig.pid();
    stream->cancel();
    REQUIRE(QTest::qWaitFor([&] { return !rig.log(u"interrupt"_s).isEmpty(); }, kWaitMs));
    QTest::qWait(1'200);
    CHECK(alive(pid));                                          // still within its grace
    CHECK(QTest::qWaitFor([&] { return !alive(pid); }, kWaitMs));
}

TEST_CASE("A cancel before the turn began sends nothing", "[codex]") {
    CodexRig rig({entry(R"({"text":"never asked"})"), entry(R"({"text":"after"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    provider.chat(ask({user(u"first"_s)}))->cancel();
    const Outcome next = run(provider, ask({user(u"second"_s)}));
    REQUIRE_FALSE(next.error);
    CHECK(next.text == u"never asked"_s);                       // the script's first entry went to the one that was sent
    REQUIRE(rig.turns().size() == 1);
    CHECK(rig.turns().first().endsWith(u"second"_s));
}

TEST_CASE("A stream dropped without a cancel stops its turn", "[codex]") {
    CodexRig rig({entry(R"({"hang":true})"), entry(R"({"text":"after"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    {
        std::shared_ptr<ChatStream> stream = provider.chat(ask({user(u"first"_s)}));
        REQUIRE(QTest::qWaitFor([&] { return rig.turns().size() == 1; }, kWaitMs));
    }
    REQUIRE(QTest::qWaitFor([&] { return !rig.log(u"interrupt"_s).isEmpty(); }, kWaitMs));
    QTest::qWait(300);
    const Outcome next = run(provider, ask({user(u"first"_s), assistant(QString()), user(u"second"_s)}));
    REQUIRE_FALSE(next.error);
    CHECK(next.text == u"after"_s);
    CHECK(rig.requests(u"thread/start"_s).size() == 2);
}

TEST_CASE("A request right after a cancel does not wait for the old turn to wind down", "[codex]") {
    CodexRig rig({entry(R"({"hang":true,"ignoreInterrupt":true})"), entry(R"({"text":"after"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    std::shared_ptr<ChatStream> stream = provider.chat(ask({user(u"first"_s)}));
    REQUIRE(QTest::qWaitFor([&] { return rig.turns().size() == 1; }, kWaitMs));
    stream->cancel();
    const Outcome next = run(provider, ask({user(u"first"_s), assistant(QString()), user(u"second"_s)}));
    REQUIRE_FALSE(next.error);
    CHECK(next.text == u"after"_s);
    CHECK(rig.log(u"argv"_s).size() == 2);                      // a second server; the first is let go
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid(0)); }, kWaitMs));
}

TEST_CASE("A turn Codex says nothing in for too long fails with Timeout and the server goes", "[codex]") {
    CodexRig rig({entry(R"({"hang":true,"ignoreInterrupt":true})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    provider.setTurnTimeoutMs(700);
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::Timeout);
    CHECK(ProviderChain::meansUnavailable(*outcome.error));
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid()); }, kWaitMs));
}

TEST_CASE("Thinking counts as a sign of life", "[codex]") {
    CodexRig rig({entry(R"({"think":6,"delay":0.25,"text":"worth the wait"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    provider.setTurnTimeoutMs(900);                             // shorter than the thinking, longer than a pause
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"worth the wait"_s);
}

TEST_CASE("warmUp starts the server before the first request, and the request uses it", "[codex]") {
    CodexRig rig({entry(R"({"text":"hi"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    provider.warmUp();
    REQUIRE(QTest::qWaitFor([&] { return rig.log(u"argv"_s).size() == 1; }, kWaitMs));
    REQUIRE_FALSE(run(provider, ask({user(u"hello"_s)})).error);
    CHECK(rig.log(u"argv"_s).size() == 1);
}

TEST_CASE("The provider's server ends with it", "[codex]") {
    CodexRig rig({entry(R"({"text":"hi"})")});
    qint64 pid = 0;
    {
        CodexProvider provider(rig.config(), rig.workingDir());
        REQUIRE_FALSE(run(provider, ask({user(u"hello"_s)})).error);
        pid = rig.pid();
        CHECK(alive(pid));
    }
    CHECK(QTest::qWaitFor([&] { return !alive(pid); }, kWaitMs));
}

TEST_CASE("CodexProvider::locate takes a configured program only if it can be run", "[codex]") {
    CHECK(CodexProvider::locate(u"/nonexistent/codex"_s).isEmpty());
#ifdef Q_OS_WIN
    CHECK(CodexProvider::locate(QStringLiteral(NEXA_TESTS_DIR "/fake-codex/codex.cmd")).endsWith(u"fake-codex/codex.cmd"_s));
#else
    CHECK(CodexProvider::locate(QStringLiteral(NEXA_TESTS_DIR "/fake-codex/codex")).endsWith(u"fake-codex/codex"_s));
#endif
}

TEST_CASE("A program that cannot start fails the request with NotFound, and listing models too", "[codex]") {
    CodexRig rig({});
    QTemporaryDir bin;
#ifdef Q_OS_WIN
    const QString path = bin.filePath(u"codex.exe"_s);
#else
    const QString path = bin.filePath(u"codex"_s);
#endif
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("#!/nonexistent/interpreter\n");
    file.close();
    REQUIRE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    CodexProvider provider(rig.config(path), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::NotFound);
    std::optional<Result<QStringList>> models;
    provider.listModels([&](Result<QStringList> result) { models = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return models.has_value(); }, kWaitMs));
    REQUIRE_FALSE(models->isOk());
    CHECK(models->error().code == ErrorCode::NotFound);
    // And the next request tries again with a fresh server rather than waiting on the dead one.
    const Outcome again = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(again.error);
    CHECK(again.error->code == ErrorCode::NotFound);
}

TEST_CASE("A second request while the first is still being stopped for its own tool does not orphan the first", "[codex]") {
    // Codex ignores the interrupt for 1.5 s, then carries on; the reminder that follows is answered "second".
    CodexRig rig({entry(R"({"tool":"commandExecution","ignoreInterrupt":true})"), entry(R"({"text":"second"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    Outcome first;
    std::shared_ptr<ChatStream> stream = provider.chat(ask({user(u"first"_s)}));
    QObject::connect(stream.get(), &ChatStream::textDelta, [&](const QString &delta) { first.text += delta; });
    QObject::connect(stream.get(), &ChatStream::finished, [&](FinishReason reason) { first.finish = reason; });
    QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &error) { first.error = error; });
    REQUIRE(QTest::qWaitFor([&] { return !rig.log(u"interrupt"_s).isEmpty(); }, kWaitMs));   // being stopped now

    const Outcome refused = run(provider, ask({user(u"first"_s), assistant(QString()), user(u"again"_s)}));
    REQUIRE(refused.error);
    CHECK(refused.error->message.contains(u"still answering"_s));

    REQUIRE(QTest::qWaitFor([&] { return first.finish || first.error; }, kWaitMs));         // the first one ends, too
    CHECK_FALSE(first.error);
    CHECK(first.text == u"second"_s);
    CHECK(rig.log(u"argv"_s).size() == 1);                      // and no second server was started for the refused one
}

TEST_CASE("An interrupt before the turn has a number goes out as soon as it has one", "[codex]") {
    CodexRig rig({entry(R"({"hang":true})")}, QJsonObject{{u"turnStartDelay"_s, 0.6}});
    CodexSession session(rig.config().baseUrl, rig.workingDir());
    int finished = 0;
    int failed = 0;
    int settled = 0;
    QObject::connect(&session, &CodexSession::turnFinished, [&](const CodexTurnResult &) { ++finished; });
    QObject::connect(&session, &CodexSession::turnFailed, [&](const Error &) { ++failed; });
    QObject::connect(&session, &CodexSession::settled, [&] { ++settled; });
    session.send(u"hello"_s, u"fake-fast"_s, true);
    REQUIRE(QTest::qWaitFor([&] { return !rig.requests(u"turn/start"_s).isEmpty(); }, kWaitMs));
    session.interrupt();                                        // turn/start has not been answered: no turn id yet
    CHECK(rig.log(u"interrupt"_s).isEmpty());
    REQUIRE(QTest::qWaitFor([&] { return settled > 0; }, kWaitMs));
    REQUIRE(rig.log(u"interrupt"_s).size() == 1);
    const QJsonObject interrupt = rig.log(u"interrupt"_s).first().value(u"interrupt"_s).toObject();
    CHECK(interrupt.value(u"threadId"_s).toString() == u"thread-1"_s);
    CHECK(interrupt.value(u"turnId"_s).toString() == u"turn-1"_s);
    QTest::qWait(200);
    CHECK(settled == 1);
    CHECK(finished == 0);
    CHECK(failed == 0);
    CHECK_FALSE(session.isBusy());
}

TEST_CASE("An interrupt before any thread exists starts no turn at all", "[codex]") {
    CodexRig rig({entry(R"({"text":"never sent"})")});
    CodexSession session(rig.config().baseUrl, rig.workingDir());
    int finished = 0;
    int failed = 0;
    int settled = 0;
    QObject::connect(&session, &CodexSession::turnFinished, [&](const CodexTurnResult &) { ++finished; });
    QObject::connect(&session, &CodexSession::turnFailed, [&](const Error &) { ++failed; });
    QObject::connect(&session, &CodexSession::settled, [&] { ++settled; });
    session.send(u"hello"_s, u"fake-fast"_s, true);
    session.interrupt();
    REQUIRE(QTest::qWaitFor([&] { return settled > 0; }, kWaitMs));
    QTest::qWait(300);
    CHECK(settled == 1);
    CHECK(finished == 0);
    CHECK(failed == 0);
    CHECK(rig.requests(u"turn/start"_s).isEmpty());
    CHECK(rig.turns().isEmpty());
    CHECK_FALSE(session.isBusy());
}

TEST_CASE("A turn cancelled while Codex is being stopped for its own tool lets its server go when it ends", "[codex]") {
    // Codex ignores the interrupt for 1.5 s and then carries on with the turn.
    CodexRig rig({entry(R"({"tool":"commandExecution","ignoreInterrupt":true})"), entry(R"({"text":"after"})")});
    CodexProvider provider(rig.config(), rig.workingDir());
    std::shared_ptr<ChatStream> stream = provider.chat(ask({user(u"first"_s)}));
    REQUIRE(QTest::qWaitFor([&] { return !rig.log(u"interrupt"_s).isEmpty(); }, kWaitMs));   // stopped for its tool
    stream->cancel();                                           // and now nobody wants the answer either
    const qint64 old = rig.pid(0);
    const Outcome next = run(provider, ask({user(u"first"_s), assistant(QString()), user(u"second"_s)}));
    REQUIRE_FALSE(next.error);
    CHECK(next.text == u"after"_s);
    CHECK(rig.log(u"argv"_s).size() == 2);                      // a second server for the second request
    // The first turn ends by itself; its server is then of no use to anyone, and goes, long before the idle stop.
    CHECK(QTest::qWaitFor([&] { return !alive(old); }, 6'000));
}

TEST_CASE("A turn against an invalid settings file names it, and the chain may move on", "[codex]") {
    CodexRig rig({entry(R"({"text":"never sent"})")},
                 QJsonObject{{u"configError"_s, u"/tmp/fake-home/.codex/config.toml:149:9"_s}});
    CodexProvider provider(rig.config(), rig.workingDir());
    const Outcome outcome = run(provider, ask({user(u"hello"_s)}));
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::InvalidArgument);
    CHECK(outcome.error->message.contains(u"line 149"_s));
    CHECK(ProviderChain::meansUnavailable(*outcome.error));
    CHECK(rig.turns().isEmpty());
}

#ifdef Q_OS_WIN
TEST_CASE("Windows Codex uses native dynamicTools and resumes the turn with item/tool/call results", "[codex]") {
    CodexRig rig({entry(R"({"dynamicTool":{"callId":"call-1","tool":"open_app","arguments":{"name":"Chrome"}},"text":"Opened Chrome."})")});
    CodexProvider provider(rig.config(), rig.workingDir());

    ToolSpec openApp{u"open_app"_s, u"Open an app"_s,
                     QJsonObject{{u"type"_s, u"object"_s},
                                 {u"properties"_s, QJsonObject{{u"name"_s, QJsonObject{{u"type"_s, u"string"_s}}}}},
                                 {u"required"_s, QJsonArray{u"name"_s}}}};

    ChatRequest req1 = ask({user(u"open chrome"_s)});
    req1.tools = {openApp};
    QList<ToolCall> gotCalls;
    Outcome step1;
    std::shared_ptr<ChatStream> stream1 = provider.chat(req1);
    QObject::connect(stream1.get(), &ChatStream::textDelta, [&](const QString &d) { step1.text += d; });
    QObject::connect(stream1.get(), &ChatStream::finished, [&](FinishReason r) {
        step1.finish = r;
        gotCalls = stream1->toolCalls();
    });
    QObject::connect(stream1.get(), &ChatStream::failed, [&](const Error &e) { step1.error = e; });
    REQUIRE(QTest::qWaitFor([&] { return step1.finish || step1.error; }, kWaitMs));
    REQUIRE_FALSE(step1.error);
    CHECK(step1.finish == FinishReason::ToolCalls);
    REQUIRE(gotCalls.size() == 1);
    CHECK(gotCalls.first().id == u"call-1"_s);
    CHECK(gotCalls.first().name == u"open_app"_s);
    CHECK(gotCalls.first().argumentsJson.contains(u"Chrome"_s));

    // Verify dynamicTools was registered on thread/start.
    const QList<QJsonObject> threads = rig.requests(u"thread/start"_s);
    REQUIRE(threads.size() == 1);
    const QJsonArray dyn = threads.first().value(u"dynamicTools"_s).toArray();
    REQUIRE(dyn.size() == 1);
    CHECK(dyn.first().toObject().value(u"name"_s).toString() == u"open_app"_s);
    CHECK(dyn.first().toObject().value(u"type"_s).toString() == u"function"_s);

    // Send the tool result back to resume the paused turn.
    ChatRequest req2 = req1;
    req2.messages.append(ChatMessage{.role = Role::Assistant, .text = step1.text, .toolCalls = gotCalls});
    req2.messages.append(ChatMessage{.role = Role::Tool, .text = u"Launched Google Chrome"_s, .toolCallId = u"call-1"_s, .toolName = u"open_app"_s});
    const Outcome step2 = run(provider, req2);
    REQUIRE_FALSE(step2.error);
    CHECK(step2.finish == FinishReason::Stop);
    CHECK(step2.text == u"Opened Chrome."_s);
    CHECK(rig.requests(u"turn/start"_s).size() == 1);          // same paused turn resumed, not a new turn!
}
#endif

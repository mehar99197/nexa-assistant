// core/ai/CodexSignIn against tests/fake-codex/codex, a stand-in for `codex app-server`. No real Codex is
// ever started here, and nothing signs in or out of anything real.

#include "core/ai/CodexSignIn.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>

#include <functional>
#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 10'000;
const QString kFakeEmail = u"private.person@example.invalid"_s;

// A scratch folder with the fake's state and log in it, and the environment that points the fake there.
struct CodexRig {
    explicit CodexRig(const QJsonObject &state) {
        write(state);
        qputenv("FAKE_CODEX_STATE", dir.filePath(u"state.json"_s).toUtf8());
        qputenv("FAKE_CODEX_LOG", dir.filePath(u"log.jsonl"_s).toUtf8());
    }
    ~CodexRig() {
        qunsetenv("FAKE_CODEX_STATE");
        qunsetenv("FAKE_CODEX_LOG");
    }

    void write(const QJsonObject &state) const {
        QFile file(dir.filePath(u"state.json"_s));
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(state).toJson());
    }
    QJsonObject state() const {
        QFile file(dir.filePath(u"state.json"_s));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
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
    QStringList requests() const {
        QStringList names;
        for (const QJsonObject &entry : log(u"request"_s)) names.append(entry.value(u"request"_s).toString());
        return names;
    }
    qint64 pid() const {
        const QList<QJsonObject> starts = log(u"argv"_s);
        return starts.isEmpty() ? 0 : qint64(starts.first().value(u"pid"_s).toInteger());
    }

    QString program() const { return QStringLiteral(NEXA_TESTS_DIR "/fake-codex/codex"); }
    QString workingDir() const { return dir.filePath(u"work"_s); }

    QTemporaryDir dir;
};

// Everything one operation says, as it says it.
struct Heard {
    explicit Heard(CodexSignIn &signIn) {
        // `context` ends the connections with this object, so a later Heard hears alone.
        QObject::connect(&signIn, &ISignIn::statusChecked, &context, [this](SignInStatus value) { status = value; });
        QObject::connect(&signIn, &ISignIn::methodsListed, &context, [this](QList<SignInMethod> value) { methods = value; });
        QObject::connect(&signIn, &ISignIn::linkReady, &context,
                         [this](QUrl url, QString text, bool opened) { link = {url, text, opened}; });
        QObject::connect(&signIn, &ISignIn::signedIn, &context, [this](SignInStatus value) { signedIn = value; });
        QObject::connect(&signIn, &ISignIn::signedOut, &context, [this] { signedOut = true; });
        QObject::connect(&signIn, &ISignIn::failed, &context, [this](Error value) { error = value; });
    }
    QObject context;
    bool finished() const { return status || signedIn || signedOut || error; }
    bool waitFinished() { return QTest::qWaitFor([this] { return finished(); }, kWaitMs); }

    struct Link {
        QUrl url;
        QString text;
        bool browserOpened{true};
    };
    std::optional<SignInStatus> status;
    std::optional<QList<SignInMethod>> methods;
    std::optional<Link> link;
    std::optional<SignInStatus> signedIn;
    bool signedOut{false};
    std::optional<Error> error;
};

bool alive(qint64 pid) { return pid > 0 && ::kill(pid_t(pid), 0) == 0; }

QStringList g_logged;
void collect(QtMsgType, const QMessageLogContext &, const QString &message) { g_logged.append(message); }

QJsonObject signedOutState(const QString &login = u"success"_s) {
    return QJsonObject{{u"signedIn"_s, false}, {u"plan"_s, u"free"_s}, {u"email"_s, kFakeEmail}, {u"login"_s, login}};
}

} // namespace

TEST_CASE("Codex status: signed out", "[codex]") {
    CodexRig rig(signedOutState());
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    CHECK(signIn.busy());
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.status);
    CHECK(heard.status->state == SignInStatus::State::SignedOut);
    CHECK_FALSE(signIn.busy());
    // It asked the account and nothing else: no thread (so none of the user's MCP servers starts either).
    CHECK(rig.requests() == QStringList{u"initialize"_s, u"account/read"_s});
}

TEST_CASE("Codex status: signed in shows the plan and never the email", "[codex]") {
    QJsonObject state = signedOutState();
    state.insert(u"signedIn"_s, true);
    state.insert(u"plan"_s, u"plus"_s);
    CodexRig rig(state);
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.status);
    CHECK(heard.status->state == SignInStatus::State::SignedIn);
    CHECK(heard.status->detail == u"ChatGPT · Plus"_s);
    CHECK_FALSE(heard.status->detail.contains(u"private"_s));
}

TEST_CASE("The server is started without Codex's own tools, in its own folder, and ends with the check", "[codex]") {
    CodexRig rig(signedOutState());
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    REQUIRE(heard.waitFinished());
    const QList<QJsonObject> starts = rig.log(u"argv"_s);
    REQUIRE(starts.size() == 1);
    const QJsonArray argv = starts.first().value(u"argv"_s).toArray();
    CHECK(argv.first().toString() == u"app-server"_s);
    CHECK(argv.contains(u"shell_tool"_s));
    CHECK(argv.contains(u"web_search=\"disabled\""_s));
    CHECK(starts.first().value(u"cwd"_s).toString().startsWith(rig.workingDir()));
    CHECK(starts.first().value(u"pdeathsig"_s).toInt() == SIGKILL);
    // The user's CODEX_HOME is left as it is: the sign-in is the owner's.
    CHECK(starts.first().value(u"codexHome"_s).toString() == qEnvironmentVariable("CODEX_HOME"));
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid()); }, kWaitMs));
}

TEST_CASE("Codex offers one way to sign in", "[codex]") {
    CodexRig rig(signedOutState());
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.listMethods();
    REQUIRE(QTest::qWaitFor([&] { return heard.methods.has_value(); }, kWaitMs));
    REQUIRE(heard.methods->size() == 1);
    CHECK(heard.methods->first().id == u"chatgpt"_s);
    CHECK(heard.methods->first().label == u"ChatGPT account"_s);
    CHECK_FALSE(heard.methods->first().needsCode);
    CHECK(rig.log(u"argv"_s).isEmpty());        // no program was started to say so
}

TEST_CASE("Signing in shows the link, waits for Codex, then reports the account", "[codex]") {
    CodexRig rig(signedOutState(u"success"_s));
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);

    g_logged.clear();
    const QtMessageHandler previous = qInstallMessageHandler(collect);
    QLoggingCategory::setFilterRules(u"nexa.*.debug=true"_s);
    signIn.signIn(u"chatgpt"_s);
    CHECK(signIn.busy());
    const bool done = heard.waitFinished();
    QLoggingCategory::setFilterRules(QString());
    qInstallMessageHandler(previous);

    REQUIRE(done);
    REQUIRE(heard.link);
    CHECK(heard.link->url.host() == u"auth.example.invalid"_s);
    CHECK_FALSE(heard.link->browserOpened);         // Nexa opens it
    CHECK_FALSE(heard.link->text.isEmpty());
    REQUIRE(heard.signedIn);
    CHECK(heard.signedIn->state == SignInStatus::State::SignedIn);
    CHECK(heard.signedIn->detail == u"ChatGPT · Free"_s);
    CHECK_FALSE(signIn.busy());
    CHECK(rig.state().value(u"signedIn"_s).toBool());

    const QList<QJsonObject> logins = rig.log(u"request"_s);
    bool started = false;
    for (const QJsonObject &entry : logins) {
        if (entry.value(u"request"_s).toString() != u"account/login/start") continue;
        started = true;
        CHECK(entry.value(u"params"_s).toObject().value(u"type"_s).toString() == u"chatgpt"_s);
    }
    CHECK(started);
    CHECK(rig.requests().last() == u"account/read"_s);          // the account is read again to be sure

    // Whatever was logged, the link's secret, the email and any code were not in it.
    for (const QString &line : std::as_const(g_logged)) {
        CHECK_FALSE(line.contains(u"SECRET-STATE"_s));
        CHECK_FALSE(line.contains(u"auth.example.invalid"_s));
        CHECK_FALSE(line.contains(u"private.person"_s));
    }
}

TEST_CASE("A sign-in Codex reports as failed is Unauthorized, without the link or code in the message", "[codex]") {
    CodexRig rig(signedOutState(u"failure"_s));
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Unauthorized);
    CHECK(heard.error->message.contains(u"did not finish"_s));
    CHECK_FALSE(heard.error->message.contains(u"SECRET-CODE"_s));
    CHECK_FALSE(heard.error->message.contains(u"auth.example.invalid"_s));
    CHECK_FALSE(heard.error->message.contains(u"https"_s));
    CHECK_FALSE(rig.state().value(u"signedIn"_s).toBool());
    CHECK_FALSE(signIn.busy());
}

TEST_CASE("Cancelling a sign-in tells Codex, ends the program and fails with Cancelled", "[codex]") {
    CodexRig rig(signedOutState(u"never"_s));
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(QTest::qWaitFor([&] { return heard.link.has_value(); }, kWaitMs));
    CHECK(signIn.busy());
    signIn.cancel();
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Cancelled);
    CHECK_FALSE(signIn.busy());
    CHECK_FALSE(heard.signedIn);
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid()); }, kWaitMs));
    CHECK(rig.requests().contains(u"account/login/cancel"_s));
    QJsonObject cancelled;
    for (const QJsonObject &entry : rig.log(u"request"_s)) {
        if (entry.value(u"request"_s).toString() == u"account/login/cancel") cancelled = entry.value(u"params"_s).toObject();
    }
    CHECK(cancelled.value(u"loginId"_s).toString() == u"login-1"_s);
}

TEST_CASE("A sign-in nobody finishes times out", "[codex]") {
    CodexRig rig(signedOutState(u"never"_s));
    CodexSignIn signIn(rig.program(), rig.workingDir());
    signIn.setLoginTimeoutMs(400);
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Timeout);
    CHECK(QTest::qWaitFor([&] { return !alive(rig.pid()); }, kWaitMs));
    CHECK(rig.requests().contains(u"account/login/cancel"_s));
}

TEST_CASE("Signing out asks Codex to log out", "[codex]") {
    QJsonObject state = signedOutState();
    state.insert(u"signedIn"_s, true);
    CodexRig rig(state);
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signOut();
    REQUIRE(heard.waitFinished());
    CHECK(heard.signedOut);
    CHECK_FALSE(rig.state().value(u"signedIn"_s).toBool());
    CHECK(rig.requests().contains(u"account/logout"_s));
}

TEST_CASE("One operation at a time", "[codex]") {
    CodexRig rig(signedOutState(u"never"_s));
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(QTest::qWaitFor([&] { return heard.link.has_value(); }, kWaitMs));
    // A second request is refused and leaves the first alone.
    std::optional<Error> refused;
    QObject::connect(&signIn, &ISignIn::failed, [&](Error error) { refused = error; });
    signIn.checkStatus();
    REQUIRE(QTest::qWaitFor([&] { return refused.has_value(); }, kWaitMs));
    CHECK(signIn.busy());
    signIn.cancel();
}

TEST_CASE("A method Codex does not have, and a code nobody needs, are refused", "[codex]") {
    CodexRig rig(signedOutState());
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signIn(u"google"_s);
    REQUIRE(heard.waitFinished());
    CHECK(heard.error->code == ErrorCode::InvalidArgument);
    CHECK(rig.log(u"argv"_s).isEmpty());

    heard.error.reset();
    signIn.submitCode(u"123456"_s);
    REQUIRE(heard.waitFinished());
    CHECK(heard.error->code == ErrorCode::NotSupported);
}

TEST_CASE("A missing Codex is NotFound", "[codex]") {
    CodexRig rig(signedOutState());
    for (const QString &program : {QString(), u"/nonexistent/codex"_s}) {
        CodexSignIn signIn(program, rig.workingDir());
        Heard heard(signIn);
        signIn.checkStatus();
        REQUIRE(heard.waitFinished());
        REQUIRE(heard.error);
        CHECK(heard.error->code == ErrorCode::NotFound);
        CHECK_FALSE(signIn.busy());
    }
}

TEST_CASE("Output that is not Codex's protocol is a parse error, never 'signed out'", "[codex]") {
    QJsonObject state = signedOutState();
    state.insert(u"mode"_s, u"garbage"_s);
    CodexRig rig(state);
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Parse);
    CHECK_FALSE(heard.status);
}

TEST_CASE("A server that dies saying it is not logged in is Unauthorized", "[codex]") {
    QJsonObject state = signedOutState();
    state.insert(u"mode"_s, u"die"_s);
    CodexRig rig(state);
    CodexSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Unauthorized);
}

TEST_CASE("A check can follow a check", "[codex]") {
    CodexRig rig(signedOutState());
    CodexSignIn signIn(rig.program(), rig.workingDir());
    for (int round = 0; round < 2; ++round) {
        Heard heard(signIn);
        signIn.checkStatus();
        REQUIRE(heard.waitFinished());
        REQUIRE(heard.status);
        CHECK(heard.status->state == SignInStatus::State::SignedOut);
    }
    CHECK(rig.log(u"argv"_s).size() == 2);
}

namespace {

// A file that is executable but cannot run: its interpreter is not there, so the start itself fails.
QString unrunnableProgram(const QTemporaryDir &dir) {
    const QString path = dir.filePath(u"codex"_s);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("#!/nonexistent/interpreter\n");
    file.close();
    REQUIRE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    return path;
}

} // namespace

TEST_CASE("A Codex that cannot start fails every operation, once and without a crash", "[codex]") {
    QTemporaryDir dir;
    const QString program = unrunnableProgram(dir);
    CodexSignIn signIn(program, dir.filePath(u"work"_s));
    const std::function<void()> operations[] = {[&] { signIn.checkStatus(); }, [&] { signIn.signIn(); },
                                                [&] { signIn.signOut(); }};
    for (const auto &operation : operations) {
        Heard heard(signIn);
        int failures = 0;
        QObject::connect(&signIn, &ISignIn::failed, &heard.context, [&](Error) { ++failures; });
        operation();
        REQUIRE(heard.waitFinished());
        REQUIRE(heard.error);
        CHECK(heard.error->code == ErrorCode::NotFound);
        CHECK_FALSE(signIn.busy());
        QTest::qWait(100);
        CHECK(failures == 1);
        CHECK_FALSE(heard.link);
    }
}

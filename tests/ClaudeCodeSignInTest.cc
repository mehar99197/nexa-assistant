// core/ai/ClaudeCodeSignIn against tests/fake-claude/claude, a stand-in for Claude Code's `auth` commands. No real
// Claude Code is ever started here, and nothing signs in or out of anything real.

#include "core/ai/ClaudeCodeSignIn.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>

#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 10'000;
const QString kFakeEmail = u"private.person@example.invalid"_s;

// A scratch folder with the fake's sign-in file and log in it, and the environment that points the fake there.
struct ClaudeAuthRig {
    explicit ClaudeAuthRig(const QString &plan = {}) {
        if (!plan.isEmpty()) signIn(plan);
        qputenv("FAKE_CLAUDE_AUTH", authFile().toUtf8());
        qputenv("FAKE_CLAUDE_LOG", dir.filePath(u"log.jsonl"_s).toUtf8());
    }
    ~ClaudeAuthRig() {
        for (const char *name : {"FAKE_CLAUDE_AUTH", "FAKE_CLAUDE_LOG", "FAKE_CLAUDE_LOGIN", "FAKE_CLAUDE_LOGIN_DELAY",
                                 "FAKE_CLAUDE_PLAN", "FAKE_CLAUDE_STATUS", "FAKE_CLAUDE_LOGOUT"}) {
            qunsetenv(name);
        }
    }

    QString authFile() const { return dir.filePath(u"auth"_s); }
    void signIn(const QString &plan) const {
        QFile file(authFile());
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(plan.toUtf8());
    }
    bool signedIn() const { return QFile::exists(authFile()) && QFile(authFile()).size() > 0; }
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
    QStringList commands() const {
        QStringList names;
        for (const QJsonObject &entry : log(u"auth"_s)) names.append(entry.value(u"auth"_s).toString());
        return names;
    }

    QString program() const { return QStringLiteral(NEXA_TESTS_DIR "/fake-claude/claude"); }
    QString workingDir() const { return dir.filePath(u"work"_s); }

    QTemporaryDir dir;
};

// Everything one operation says, as it says it.
struct Heard {
    explicit Heard(ClaudeCodeSignIn &signIn) {
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
    bool waitLink() { return QTest::qWaitFor([this] { return link.has_value() || error.has_value(); }, kWaitMs); }

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

// Every log line Nexa wrote while `act` ran, with the ai category's debug lines on.
QStringList loggedDuring(const std::function<void()> &act) {
    g_logged.clear();
    QLoggingCategory::setFilterRules(u"nexa.ai.debug=true"_s);
    const QtMessageHandler previous = qInstallMessageHandler(collect);
    act();
    qInstallMessageHandler(previous);
    QLoggingCategory::setFilterRules(QString());
    return g_logged;
}

} // namespace

TEST_CASE("Claude Code status: signed out", "[claude]") {
    ClaudeAuthRig rig;
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    CHECK(signIn.busy());
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.status);
    CHECK(heard.status->state == SignInStatus::State::SignedOut);
    CHECK_FALSE(signIn.busy());
    CHECK(rig.commands() == QStringList{u"status"_s});
}

TEST_CASE("Claude Code status: signed in shows the plan and never the email, in words or in the log", "[claude]") {
    ClaudeAuthRig rig(u"max"_s);
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    const QStringList logged = loggedDuring([&] {
        signIn.checkStatus();
        REQUIRE(heard.waitFinished());
    });
    REQUIRE(heard.status);
    CHECK(heard.status->state == SignInStatus::State::SignedIn);
    CHECK(heard.status->detail == u"Claude · Max"_s);
    CHECK_FALSE(heard.status->detail.contains(u"private"_s));
    for (const QString &line : logged) {
        INFO(line.toStdString());
        CHECK_FALSE(line.contains(kFakeEmail));
        CHECK_FALSE(line.contains(u"SECRET"_s));
    }
}

TEST_CASE("Claude Code offers one way to sign in, which may ask for a code", "[claude]") {
    ClaudeAuthRig rig;
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.listMethods();
    REQUIRE(QTest::qWaitFor([&heard] { return heard.methods.has_value(); }, kWaitMs));
    REQUIRE(heard.methods->size() == 1);
    CHECK(heard.methods->first().id == u"claudeai"_s);
    CHECK(heard.methods->first().needsCode);
    CHECK_FALSE(signIn.busy());
}

TEST_CASE("Signing in shows the link, takes the code, then reports what Claude Code now says", "[claude]") {
    ClaudeAuthRig rig;
    qputenv("FAKE_CLAUDE_LOGIN", "code");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    const QStringList logged = loggedDuring([&] {
        signIn.signIn();
        REQUIRE(heard.waitLink());
        REQUIRE(heard.link);
        CHECK(heard.link->url.host() == u"claude.example.invalid"_s);
        CHECK(heard.link->browserOpened);
        CHECK(heard.link->text.contains(u"browser"_s));
        CHECK(signIn.pendingNeedsCode());
        signIn.submitCode(u" 123456 "_s);
        REQUIRE(heard.waitFinished());
    });
    REQUIRE(heard.signedIn);
    CHECK(heard.signedIn->state == SignInStatus::State::SignedIn);
    CHECK(heard.signedIn->detail == u"Claude · Max"_s);
    CHECK_FALSE(signIn.busy());
    CHECK_FALSE(signIn.pendingNeedsCode());
    CHECK(rig.commands() == QStringList{u"login"_s, u"status"_s});
    const QList<QJsonObject> codes = rig.log(u"codeReceived"_s);
    REQUIRE(codes.size() == 1);
    CHECK(codes.first().value(u"codeReceived"_s).toString() == u"123456"_s);
    // Neither the link nor the code nor the account reaches the log.
    for (const QString &line : logged) {
        INFO(line.toStdString());
        CHECK_FALSE(line.contains(u"oauth"_s));
        CHECK_FALSE(line.contains(u"123456"_s));
        CHECK_FALSE(line.contains(kFakeEmail));
    }
}

TEST_CASE("A sign-in the browser's callback finishes needs no code", "[claude]") {
    ClaudeAuthRig rig;
    qputenv("FAKE_CLAUDE_LOGIN", "callback");
    qputenv("FAKE_CLAUDE_PLAN", "pro");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signIn(u"claudeai"_s);
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.signedIn);
    CHECK(heard.signedIn->detail == u"Claude · Pro"_s);
    CHECK(heard.link.has_value());
    CHECK(rig.log(u"codeReceived"_s).isEmpty());
}

TEST_CASE("A sign-in Claude Code reports as failed is Unauthorized, without the email in the message", "[claude]") {
    ClaudeAuthRig rig;
    qputenv("FAKE_CLAUDE_LOGIN", "fail");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Unauthorized);
    CHECK_FALSE(heard.error->message.contains(kFakeEmail));
    CHECK_FALSE(rig.signedIn());
}

TEST_CASE("A code Claude Code refuses fails the sign-in, and nobody is signed in", "[claude]") {
    ClaudeAuthRig rig;
    qputenv("FAKE_CLAUDE_LOGIN", "code");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(heard.waitLink());
    signIn.submitCode(u"bad"_s);
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Unauthorized);
    CHECK_FALSE(heard.error->message.contains(u"bad"_s));
    CHECK_FALSE(rig.signedIn());
}

TEST_CASE("Cancelling a sign-in ends the program and fails with Cancelled", "[claude]") {
    ClaudeAuthRig rig;
    qputenv("FAKE_CLAUDE_LOGIN", "never");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(heard.waitLink());
    const QList<QJsonObject> logins = rig.log(u"auth"_s);
    REQUIRE_FALSE(logins.isEmpty());
    const qint64 pid = qint64(logins.last().value(u"pid"_s).toInteger());
    CHECK(alive(pid));
    signIn.cancel();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Cancelled);
    CHECK_FALSE(signIn.busy());
    CHECK(QTest::qWaitFor([pid] { return !alive(pid); }, kWaitMs));
}

TEST_CASE("A sign-in nobody finishes times out", "[claude]") {
    ClaudeAuthRig rig;
    qputenv("FAKE_CLAUDE_LOGIN", "never");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    signIn.setTimeouts(5'000, 400);
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Timeout);
    CHECK_FALSE(signIn.busy());
}

TEST_CASE("Signing out asks Claude Code to log out, and trusts what it says afterwards", "[claude]") {
    ClaudeAuthRig rig(u"max"_s);
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    {
        Heard heard(signIn);
        signIn.signOut();
        REQUIRE(heard.waitFinished());
        CHECK(heard.signedOut);
        CHECK_FALSE(rig.signedIn());
        CHECK(rig.commands() == QStringList{u"logout"_s, u"status"_s});
    }
    // A logout that changed nothing is not "signed out".
    rig.signIn(u"max"_s);
    qputenv("FAKE_CLAUDE_LOGOUT", "no-effect");
    Heard heard(signIn);
    signIn.signOut();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK_FALSE(heard.signedOut);
    CHECK(rig.signedIn());
}

TEST_CASE("One Claude Code operation at a time", "[claude]") {
    ClaudeAuthRig rig(u"max"_s);
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    std::optional<Error> refused;
    QObject context;
    QObject::connect(&signIn, &ISignIn::failed, &context, [&refused](Error error) { refused = error; });
    signIn.checkStatus();       // at once, while the first still runs
    REQUIRE(refused);
    CHECK(refused->code == ErrorCode::InvalidArgument);
    CHECK(signIn.busy());
    // The refusal is a failure too (heard.error): the first operation still answers.
    REQUIRE(QTest::qWaitFor([&heard] { return heard.status.has_value(); }, kWaitMs));
    CHECK(heard.status->state == SignInStatus::State::SignedIn);
}

TEST_CASE("A way to sign in Claude Code does not have, and a code nobody needs, are refused", "[claude]") {
    ClaudeAuthRig rig;
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    {
        Heard heard(signIn);
        signIn.signIn(u"google"_s);
        REQUIRE(heard.waitFinished());
        REQUIRE(heard.error);
        CHECK(heard.error->code == ErrorCode::InvalidArgument);
        CHECK_FALSE(signIn.busy());
    }
    Heard heard(signIn);
    signIn.submitCode(u"123456"_s);
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::InvalidArgument);
    CHECK(rig.commands().isEmpty());
}

TEST_CASE("A missing Claude Code is NotFound", "[claude]") {
    ClaudeCodeSignIn signIn{QString()};
    Heard heard(signIn);
    signIn.checkStatus();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::NotFound);
    CHECK(heard.error->message.contains(u"not installed"_s));
}

TEST_CASE("A status that is not Claude Code's JSON is a parse error, never 'signed out'", "[claude]") {
    ClaudeAuthRig rig(u"max"_s);
    qputenv("FAKE_CLAUDE_STATUS", "garbage");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::Parse);
    CHECK_FALSE(heard.status.has_value());
}

TEST_CASE("A status command that dies before answering is ConnectionRefused", "[claude]") {
    ClaudeAuthRig rig(u"max"_s);
    qputenv("FAKE_CLAUDE_STATUS", "die");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    Heard heard(signIn);
    signIn.checkStatus();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.error);
    CHECK(heard.error->code == ErrorCode::ConnectionRefused);
}

TEST_CASE("A check can follow a check, and a sign-in a check", "[claude]") {
    ClaudeAuthRig rig;
    qputenv("FAKE_CLAUDE_LOGIN", "callback");
    ClaudeCodeSignIn signIn(rig.program(), rig.workingDir());
    for (int i = 0; i < 2; ++i) {
        Heard heard(signIn);
        signIn.checkStatus();
        REQUIRE(heard.waitFinished());
        REQUIRE(heard.status);
        CHECK(heard.status->state == SignInStatus::State::SignedOut);
    }
    Heard heard(signIn);
    signIn.signIn();
    REQUIRE(heard.waitFinished());
    REQUIRE(heard.signedIn);
    CHECK(rig.commands() == QStringList{u"status"_s, u"status"_s, u"login"_s, u"status"_s});
}

// core/ai/AgySignIn against tests/fake-agy/agy in its `acp` mode, a stand-in for `agy acp` (Google's
// Antigravity CLI as an Agent Client Protocol server). Nothing here starts the real agy, signs anyone in
// or out, or touches a keyring: the fake's "keyring" is a file in the test's own folder.

#include "core/ai/AgySignIn.h"
#include "core/ProcessUtils.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 15'000;

// A scratch folder with the fake's keyring and log in it, and the environment that points the fake there.
// `script` is $FAKE_AGY_ACP (see the fake), `signedInAs` the plan of whoever is already signed in.
struct AcpRig {
    explicit AcpRig(const QJsonObject &script = {}, const QString &signedInAs = {}) {
        qputenv("FAKE_AGY_ACP", QJsonDocument(script).toJson(QJsonDocument::Compact));
        qputenv("FAKE_AGY_KEYRING", keyringPath().toUtf8());
        qputenv("FAKE_AGY_LOG", dir.filePath(u"log.jsonl"_s).toUtf8());
        if (!signedInAs.isEmpty()) {
            QFile keyring(keyringPath());
            REQUIRE(keyring.open(QIODevice::WriteOnly));
            keyring.write(signedInAs.toUtf8());
        }
    }
    ~AcpRig() {
        qunsetenv("FAKE_AGY_ACP");
        qunsetenv("FAKE_AGY_KEYRING");
        qunsetenv("FAKE_AGY_LOG");
    }

    QString program() const {
#ifdef Q_OS_WIN
        return QStringLiteral(NEXA_TESTS_DIR "/fake-agy/agy.cmd");
#else
        return QStringLiteral(NEXA_TESTS_DIR "/fake-agy/agy");
#endif
    }
    QString workingDir() const { return dir.filePath(u"work"_s); }
    QString keyringPath() const { return dir.filePath(u"keyring"_s); }

    // Who the fake's keyring says is signed in; empty when nobody.
    QString keyring() const {
        QFile file(keyringPath());
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
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
    // The methods agy was asked, in order.
    QStringList requests() const {
        QStringList methods;
        for (const QJsonObject &entry : log(u"acp"_s)) methods.append(entry.value(u"acp"_s).toString());
        return methods;
    }
    QJsonObject request(const QString &method) const {
        for (const QJsonObject &entry : log(u"acp"_s)) {
            if (entry.value(u"acp"_s).toString() == method) return entry;
        }
        return {};
    }
    qint64 pid() const {
        const QList<QJsonObject> starts = log(u"argv"_s);
        return starts.isEmpty() ? 0 : qint64(starts.first().value(u"pid"_s).toInteger());
    }
    qint64 childPid() const {
        const QList<QJsonObject> children = log(u"child"_s);
        return children.isEmpty() ? 0 : qint64(children.first().value(u"child"_s).toInteger());
    }
    int starts() const { return int(log(u"argv"_s).size()); }

    QTemporaryDir dir;
};

// Every signal an operation can end in, and how to wait for the end.
struct Spies {
    explicit Spies(AgySignIn &signIn)
        : status(&signIn, &ISignIn::statusChecked), methods(&signIn, &ISignIn::methodsListed),
          link(&signIn, &ISignIn::linkReady), signedIn(&signIn, &ISignIn::signedIn),
          signedOut(&signIn, &ISignIn::signedOut), failed(&signIn, &ISignIn::failed) {}

    int ends() const {
        return int(status.count() + methods.count() + signedIn.count() + signedOut.count() + failed.count());
    }
    bool wait(int count = 1) {
        return QTest::qWaitFor([this, count] { return ends() >= count; }, kWaitMs);
    }
    Error error() const { return failed.first().first().value<Error>(); }
    SignInStatus statusValue() const { return status.first().first().value<SignInStatus>(); }
    SignInStatus signedInValue() const { return signedIn.first().first().value<SignInStatus>(); }

    QSignalSpy status, methods, link, signedIn, signedOut, failed;
};

bool alive(qint64 pid) { return process::isAlive(pid); }
bool gone(qint64 pid) { return QTest::qWaitFor([pid] { return !alive(pid); }, kWaitMs); }
bool logged(const AcpRig &rig, const QString &method) {
    return QTest::qWaitFor([&] { return rig.requests().contains(method); }, kWaitMs);
}

QStringList &capturedLog() {
    static QStringList messages;
    return messages;
}

// Everything nexa.ai logs while it lives, at debug level.
struct LogCapture {
    LogCapture() {
        capturedLog().clear();
        QLoggingCategory::setFilterRules(u"nexa.ai.debug=true"_s);
        previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            capturedLog().append(message);
        });
    }
    ~LogCapture() {
        qInstallMessageHandler(previous);
        QLoggingCategory::setFilterRules(QString());
    }
    QString text() const { return capturedLog().join(u'\n'); }

    QtMessageHandler previous{nullptr};
};

} // namespace

// -- status -----------------------------------------------------------------------------------------------

TEST_CASE("Status: an authentication error from session/new is signed out, and nothing is left running",
          "[agy-signin]") {
    AcpRig rig;
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    CHECK(signIn.busy());
    REQUIRE(spies.wait());
    REQUIRE(spies.status.count() == 1);
    CHECK(spies.failed.count() == 0);
    CHECK(spies.statusValue().state == SignInStatus::State::SignedOut);
    CHECK(spies.statusValue().detail.isEmpty());
    CHECK_FALSE(signIn.busy());
    CHECK(rig.requests() == QStringList{u"initialize"_s, u"session/new"_s});    // no session to close
    CHECK(gone(rig.pid()));
}

TEST_CASE("Status: a session means signed in, with the plan, and the session is closed again", "[agy-signin]") {
    AcpRig rig({}, u"Google AI Pro"_s);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    REQUIRE(spies.status.count() == 1);
    CHECK(spies.statusValue().state == SignInStatus::State::SignedIn);
    CHECK(spies.statusValue().detail == u"Google AI Pro"_s);
    CHECK(rig.requests() == QStringList{u"initialize"_s, u"session/new"_s, u"session/close"_s});

    const QJsonObject init = rig.request(u"initialize"_s).value(u"params"_s).toObject();
    CHECK(init.value(u"protocolVersion"_s).toInt() == 1);
    CHECK(init.value(u"clientInfo"_s).toObject().value(u"name"_s).toString() == u"nexa"_s);
    const QJsonObject open = rig.request(u"session/new"_s).value(u"params"_s).toObject();
    CHECK(open.value(u"cwd"_s).toString() == rig.workingDir() + u"/work"_s);
    CHECK(open.value(u"mcpServers"_s).toArray().isEmpty());
    const QJsonObject close = rig.request(u"session/close"_s).value(u"params"_s).toObject();
    CHECK(close.value(u"sessionId"_s).toString() == u"fake-session-%1"_s.arg(rig.pid()));
    CHECK(gone(rig.pid()));
}

TEST_CASE("agy runs as `acp` in Nexa's own home and workspace, and dies with Nexa", "[agy-signin]") {
    AcpRig rig;
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());

    const QList<QJsonObject> starts = rig.log(u"argv"_s);
    REQUIRE(starts.size() == 1);
    const QJsonObject start = starts.first();
    REQUIRE(start.value(u"argv"_s).toArray().size() == 1);
    CHECK(start.value(u"argv"_s).toArray().first().toString() == u"acp"_s);
    CHECK(start.value(u"home"_s).toString() == rig.workingDir() + u"/home"_s);
    CHECK(start.value(u"cwd"_s).toString().startsWith(rig.workingDir() + u"/work"_s));
#ifdef Q_OS_UNIX
    CHECK(start.value(u"pdeathsig"_s).toInt() == SIGKILL);
#endif
    // the deny rules AgySession::prepare writes, so agy's own tools stay off
    const QJsonArray deny =
        start.value(u"settings"_s).toObject().value(u"permissions"_s).toObject().value(u"deny"_s).toArray();
    CHECK(deny.contains(u"command(*)"_s));
    CHECK(deny.contains(u"read_file(*)"_s));
}

TEST_CASE("A program that is not there is NotFound, whether it is unnamed or named wrongly", "[agy-signin]") {
    AcpRig rig;
    for (const QString &program : {QString(), u"/nonexistent/agy"_s}) {
        AgySignIn signIn(program, rig.workingDir());
        Spies spies(signIn);
        signIn.checkStatus();
        REQUIRE(spies.wait());
        REQUIRE(spies.failed.count() == 1);
        CHECK(spies.error().code == ErrorCode::NotFound);
        CHECK(spies.error().message.contains(u"agy is not installed"_s));
        CHECK_FALSE(signIn.busy());
    }
}

// -- methods ----------------------------------------------------------------------------------------------

TEST_CASE("The sign-in methods are the browser ones agy lists, labelled for Settings", "[agy-signin]") {
    AcpRig rig;
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.listMethods();
    REQUIRE(spies.wait());
    REQUIRE(spies.methods.count() == 1);
    const QList<SignInMethod> methods = spies.methods.first().first().value<QList<SignInMethod>>();
    REQUIRE(methods.size() == 2);
    CHECK(methods.at(0).id == u"oauth-personal"_s);
    CHECK(methods.at(0).label == u"Google account"_s);
    CHECK_FALSE(methods.at(0).needsCode);
    CHECK(methods.at(1).id == u"oauth-business"_s);
    CHECK(methods.at(1).label == u"Gemini Enterprise"_s);
    CHECK(rig.requests() == QStringList{u"initialize"_s});
    CHECK(gone(rig.pid()));
}

// -- sign in ----------------------------------------------------------------------------------------------

TEST_CASE("Sign in waits for agy's answer, then reports what agy now says", "[agy-signin]") {
    AcpRig rig({{u"delayMs"_s, 400}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signIn();
    CHECK(signIn.busy());
    QTest::qWait(150);
    CHECK(signIn.busy());               // still waiting for the browser
    CHECK(spies.ends() == 0);
    REQUIRE(spies.wait());
    REQUIRE(spies.signedIn.count() == 1);
    CHECK(spies.failed.count() == 0);
    CHECK(spies.link.count() == 0);     // agy opened the browser itself: nothing for Nexa to show
    CHECK(spies.signedInValue().state == SignInStatus::State::SignedIn);
    CHECK(spies.signedInValue().detail == u"Fake AI Pro"_s);
    CHECK_FALSE(signIn.busy());
    CHECK(rig.keyring() == u"Fake AI Pro"_s);
    // the default method, then the same check a status request makes, and the session closed again
    CHECK(rig.requests()
          == QStringList{u"initialize"_s, u"authenticate"_s, u"session/new"_s, u"session/close"_s});
    CHECK(rig.request(u"authenticate"_s).value(u"params"_s).toObject().value(u"methodId"_s).toString()
          == u"oauth-personal"_s);
    CHECK(gone(rig.pid()));
}

TEST_CASE("Sign in names the method it was asked for", "[agy-signin]") {
    AcpRig rig({{u"plan"_s, u"Gemini Enterprise"_s}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signIn(u"oauth-business"_s);
    REQUIRE(spies.wait());
    REQUIRE(spies.signedIn.count() == 1);
    CHECK(spies.signedInValue().detail == u"Gemini Enterprise"_s);
    CHECK(rig.request(u"authenticate"_s).value(u"params"_s).toObject().value(u"methodId"_s).toString()
          == u"oauth-business"_s);
}

TEST_CASE("Only a browser sign-in can be asked for, and agy's own refusal is reported", "[agy-signin]") {
    AcpRig rig;
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    for (const QString &id : {u"gemini-api-key"_s, u"oauth-x; rm -rf ~"_s, u"../oauth-personal"_s}) {
        signIn.signIn(id);
        REQUIRE(spies.wait(spies.ends() + 1));
        CHECK(spies.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(signIn.busy());
    }
    CHECK(rig.starts() == 0);           // nothing was started for any of them

    // An id of the right shape that agy does not know: agy says -32602.
    Spies fresh(signIn);
    signIn.signIn(u"oauth-nonexistent"_s);
    REQUIRE(fresh.wait());
    REQUIRE(fresh.failed.count() == 1);
    CHECK(fresh.error().code == ErrorCode::InvalidArgument);
    CHECK(fresh.error().message.contains(u"does not offer"_s));
    CHECK(gone(rig.pid()));
}

TEST_CASE("An error from authenticate fails the sign-in without an account in the words", "[agy-signin]") {
    AcpRig rig({{u"authenticate"_s, u"error"_s}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signIn();
    REQUIRE(spies.wait());
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.signedIn.count() == 0);
    CHECK(spies.error().code == ErrorCode::Unauthorized);
    CHECK(spies.error().message.contains(u"agy could not sign in"_s));
    CHECK(spies.error().message.contains(u"cancelled"_s));
    CHECK_FALSE(spies.error().message.contains(u"someone"_s));
    CHECK_FALSE(spies.error().message.contains(u"@"_s));
    CHECK(rig.keyring().isEmpty());
    CHECK(gone(rig.pid()));
}

TEST_CASE("A sign-in agy answers but did not make is not reported as signed in", "[agy-signin]") {
    AcpRig rig({{u"authenticate"_s, u"no-effect"_s}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signIn();
    REQUIRE(spies.wait());
    CHECK(spies.signedIn.count() == 0);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::Unauthorized);
    CHECK(spies.error().message.contains(u"still reports"_s));
}

TEST_CASE("A login link agy hands over is passed on, with the browser not yet opened", "[agy-signin]") {
    AcpRig rig({{u"authenticate"_s, u"link"_s}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signIn();
    REQUIRE(spies.wait());
    REQUIRE(spies.link.count() == 1);
    CHECK(spies.link.first().at(0).toUrl() == QUrl(u"https://accounts.example.invalid/signin?fake=1"_s));
    CHECK_FALSE(spies.link.first().at(1).toString().isEmpty());
    CHECK_FALSE(spies.link.first().at(2).toBool());
    CHECK(spies.signedIn.count() == 1);
}

TEST_CASE("Cancel ends a sign-in at once, with the whole process group", "[agy-signin]") {
    AcpRig rig({{u"hang"_s, u"authenticate"_s}, {u"child"_s, true}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signIn();
    REQUIRE(logged(rig, u"authenticate"_s));
    REQUIRE(QTest::qWaitFor([&] { return rig.childPid() != 0; }, kWaitMs));
    const qint64 agy = rig.pid();
    const qint64 child = rig.childPid();
    REQUIRE(alive(agy));
    REQUIRE(alive(child));

    signIn.cancel();
    REQUIRE(spies.failed.count() == 1);             // at once, not on the next turn of the loop
    CHECK(spies.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(signIn.busy());
    CHECK(gone(agy));
    CHECK(gone(child));                             // agy's own children too: the language server
    QTest::qWait(300);
    CHECK(spies.ends() == 1);                       // and nothing more, the process's death included
    CHECK(rig.keyring().isEmpty());
}

TEST_CASE("Cancel with nothing running does nothing", "[agy-signin]") {
    AcpRig rig;
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.cancel();
    QTest::qWait(50);
    CHECK(spies.ends() == 0);
    CHECK_FALSE(signIn.busy());
}

TEST_CASE("A sign-in that takes longer than allowed times out and ends agy", "[agy-signin]") {
    AcpRig rig({{u"hang"_s, u"authenticate"_s}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    signIn.setTimeouts(5'000, 500);
    Spies spies(signIn);
    signIn.signIn();
    REQUIRE(spies.wait());
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::Timeout);
    CHECK(spies.error().message.contains(u"nobody was signed in"_s));
    CHECK_FALSE(signIn.busy());
    CHECK(gone(rig.pid()));
}

TEST_CASE("agy that does not answer a status request in time is a timeout, not signed out", "[agy-signin]") {
    for (const QString &method : {u"initialize"_s, u"session/new"_s}) {
        AcpRig rig({{u"hang"_s, method}});
        AgySignIn signIn(rig.program(), rig.workingDir());
        signIn.setTimeouts(500, 5'000);
        Spies spies(signIn);
        signIn.checkStatus();
        REQUIRE(spies.wait());
        CHECK(spies.status.count() == 0);
        REQUIRE(spies.failed.count() == 1);
        CHECK(spies.error().code == ErrorCode::Timeout);
        CHECK(gone(rig.pid()));
    }
}

// -- what agy says that Nexa cannot read ------------------------------------------------------------------

TEST_CASE("Output that is not JSON-RPC is a Parse failure, never signed out", "[agy-signin]") {
    for (const QString &method : {u"initialize"_s, u"session/new"_s}) {
        AcpRig rig({{u"garbage"_s, method}});
        AgySignIn signIn(rig.program(), rig.workingDir());
        Spies spies(signIn);
        signIn.checkStatus();
        REQUIRE(spies.wait());
        CHECK(spies.status.count() == 0);
        REQUIRE(spies.failed.count() == 1);
        CHECK(spies.error().code == ErrorCode::Parse);
        CHECK_FALSE(signIn.busy());
        CHECK(gone(rig.pid()));
    }
}

TEST_CASE("An initialize reply that is not one is a Parse failure", "[agy-signin]") {
    AcpRig rig({{u"badInitialize"_s, true}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    CHECK(spies.status.count() == 0);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::Parse);
    CHECK(rig.requests() == QStringList{u"initialize"_s});
    CHECK(gone(rig.pid()));
}

TEST_CASE("An error that is not \"authentication required\" is not signed out either", "[agy-signin]") {
    for (const QString &method : {u"initialize"_s, u"session/new"_s}) {
        AcpRig rig({{u"error"_s, method}});
        AgySignIn signIn(rig.program(), rig.workingDir());
        Spies spies(signIn);
        signIn.checkStatus();
        REQUIRE(spies.wait());
        CHECK(spies.status.count() == 0);
        REQUIRE(spies.failed.count() == 1);
        CHECK(spies.error().code == ErrorCode::Unknown);
        CHECK(spies.error().message.contains(u"agy"_s));
        CHECK(spies.error().message.contains(u"-32000"_s));             // the code, to tell the cause apart
        CHECK(gone(rig.pid()));
    }
}

TEST_CASE("A session reply with no session id is a Parse failure, not a plan", "[agy-signin]") {
    AcpRig rig({{u"noSessionId"_s, true}}, u"Google AI Pro"_s);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    CHECK(spies.status.count() == 0);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::Parse);
}

TEST_CASE("agy ending before it answers is reported with its exit code", "[agy-signin]") {
    AcpRig rig({{u"die"_s, u"session/new"_s}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    CHECK(spies.status.count() == 0);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::ConnectionRefused);
    CHECK(spies.error().message.contains(u"exit 3"_s));
}

TEST_CASE("A request from agy is refused, so agy does not wait for an answer", "[agy-signin]") {
    AcpRig rig({{u"agentRequest"_s, u"initialize"_s}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    REQUIRE(spies.status.count() == 1);
    const QList<QJsonObject> replies = rig.log(u"clientReply"_s);
    REQUIRE(replies.size() == 1);
    const QJsonObject reply = replies.first().value(u"clientReply"_s).toObject();
    CHECK(reply.value(u"id"_s).toInt() == 99);
    CHECK(reply.value(u"error"_s).toObject().value(u"code"_s).toInt() == -32601);
}

// -- sign out ---------------------------------------------------------------------------------------------

TEST_CASE("Sign out logs out, then checks that agy agrees", "[agy-signin]") {
    AcpRig rig({}, u"Google AI Pro"_s);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signOut();
    CHECK(signIn.busy());
    REQUIRE(spies.wait());
    CHECK(spies.signedOut.count() == 1);
    CHECK(spies.failed.count() == 0);
    CHECK_FALSE(signIn.busy());
    CHECK(rig.keyring().isEmpty());
    CHECK(rig.requests() == QStringList{u"initialize"_s, u"logout"_s, u"session/new"_s});
    CHECK(gone(rig.pid()));
}

TEST_CASE("An agy without the logout capability is not asked to log out", "[agy-signin]") {
    AcpRig rig({{u"canLogout"_s, false}}, u"Google AI Pro"_s);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signOut();
    REQUIRE(spies.wait());
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::NotSupported);
    CHECK(rig.requests() == QStringList{u"initialize"_s});
    CHECK(rig.keyring() == u"Google AI Pro"_s);
    CHECK(gone(rig.pid()));
}

TEST_CASE("A logout that changed nothing is not reported as signed out", "[agy-signin]") {
    AcpRig rig({{u"logout"_s, u"no-effect"_s}}, u"Google AI Pro"_s);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signOut();
    REQUIRE(spies.wait());
    CHECK(spies.signedOut.count() == 0);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().message.contains(u"still signed in"_s));
    CHECK(rig.requests().last() == u"session/close"_s);     // the session it opened to check is closed
    CHECK(rig.keyring() == u"Google AI Pro"_s);
}

TEST_CASE("An error from logout fails the sign-out", "[agy-signin]") {
    AcpRig rig({{u"logout"_s, u"error"_s}}, u"Google AI Pro"_s);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signOut();
    REQUIRE(spies.wait());
    CHECK(spies.signedOut.count() == 0);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().message.contains(u"could not sign out"_s));
    CHECK(rig.keyring() == u"Google AI Pro"_s);
}

// -- one at a time ----------------------------------------------------------------------------------------

TEST_CASE("A second operation while one runs fails at once and leaves the first alone", "[agy-signin]") {
    AcpRig rig({{u"delayMs"_s, 400}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.signIn();
    REQUIRE(signIn.busy());

    signIn.checkStatus();
    REQUIRE(spies.failed.count() == 1);             // immediately, from inside the call
    CHECK(spies.error().code == ErrorCode::InvalidArgument);
    signIn.signOut();
    signIn.listMethods();
    signIn.signIn(u"oauth-business"_s);
    CHECK(spies.failed.count() == 4);
    CHECK(signIn.busy());

    REQUIRE(spies.wait(5));
    CHECK(spies.signedIn.count() == 1);             // the first one went on and finished
    CHECK(spies.failed.count() == 4);
    CHECK(rig.starts() == 1);
    CHECK(rig.keyring() == u"Fake AI Pro"_s);
}

TEST_CASE("One AgySignIn carries out operation after operation", "[agy-signin]") {
    AcpRig rig;
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);

    signIn.checkStatus();
    REQUIRE(spies.wait(1));
    REQUIRE(spies.status.count() == 1);
    CHECK(spies.statusValue().state == SignInStatus::State::SignedOut);

    signIn.signIn();
    REQUIRE(spies.wait(2));
    REQUIRE(spies.signedIn.count() == 1);

    signIn.checkStatus();
    REQUIRE(spies.wait(3));
    REQUIRE(spies.status.count() == 2);
    CHECK(spies.status.at(1).first().value<SignInStatus>().state == SignInStatus::State::SignedIn);
    CHECK(spies.status.at(1).first().value<SignInStatus>().detail == u"Fake AI Pro"_s);

    signIn.signOut();
    REQUIRE(spies.wait(4));
    REQUIRE(spies.signedOut.count() == 1);

    signIn.checkStatus();
    REQUIRE(spies.wait(5));
    REQUIRE(spies.status.count() == 3);
    CHECK(spies.status.at(2).first().value<SignInStatus>().state == SignInStatus::State::SignedOut);
    CHECK(spies.failed.count() == 0);
    CHECK(rig.starts() == 5);                       // a program of its own for each
}

TEST_CASE("agy never asks for a pasted code", "[agy-signin]") {
    AcpRig rig;
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.submitCode(u"123456"_s);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::NotSupported);
    CHECK_FALSE(spies.error().message.contains(u"123456"_s));
    CHECK_FALSE(signIn.busy());
}

TEST_CASE("Destroying an AgySignIn in the middle of an operation ends agy and its children", "[agy-signin]") {
    AcpRig rig({{u"hang"_s, u"authenticate"_s}, {u"child"_s, true}});
    auto *signIn = new AgySignIn(rig.program(), rig.workingDir());
    signIn->signIn();
    REQUIRE(logged(rig, u"authenticate"_s));
    REQUIRE(QTest::qWaitFor([&] { return rig.childPid() != 0; }, kWaitMs));
    const qint64 agy = rig.pid();
    const qint64 child = rig.childPid();
    delete signIn;
    CHECK(gone(agy));
    CHECK(gone(child));
}

// -- what is never logged ---------------------------------------------------------------------------------

TEST_CASE("The log holds method names and codes, never a link, an address or agy's words", "[agy-signin]") {
    LogCapture capture;
    {
        AcpRig rig({{u"authenticate"_s, u"link"_s}});
        AgySignIn signIn(rig.program(), rig.workingDir());
        Spies spies(signIn);
        signIn.signIn();
        REQUIRE(spies.wait());
        REQUIRE(spies.signedIn.count() == 1);
    }
    {
        AcpRig rig({{u"authenticate"_s, u"error"_s}});
        AgySignIn signIn(rig.program(), rig.workingDir());
        Spies spies(signIn);
        signIn.signIn();
        REQUIRE(spies.wait());
        REQUIRE(spies.failed.count() == 1);
    }
    const QString log = capture.text();
    CHECK(log.contains(u"authenticate"_s));             // the capture works, and says what was asked
    CHECK_FALSE(log.contains(u"accounts.example.invalid"_s));
    CHECK_FALSE(log.contains(u"fake=1"_s));
    CHECK_FALSE(log.contains(u"someone"_s));
    CHECK_FALSE(log.contains(u"@"_s));
    CHECK_FALSE(log.contains(u"cancelled"_s));          // agy's own words
    CHECK_FALSE(log.contains(u"Fake AI Pro"_s));
}

// -- review additions: framing, process groups, paths -----------------------------------------------------

namespace {

// Kills what a test leaves behind when a CHECK fails, so a failing run does not leave `sleep 120` around.
struct KillOnExit {
    explicit KillOnExit(const AcpRig &rig) : rig(rig) {}
    ~KillOnExit() {
        if (const qint64 child = rig.childPid(); child > 0) process::killTree(child, true, true);
        if (const qint64 agy = rig.pid(); agy > 0) process::killTree(agy, true, true);
    }
    const AcpRig &rig;
};

} // namespace

TEST_CASE("agy that ends by itself leaves nothing of its process group behind", "[agy-signin]") {
    AcpRig rig({{u"child"_s, true}, {u"die"_s, u"session/new"_s}});
    KillOnExit cleanup(rig);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::ConnectionRefused);
    const qint64 child = rig.childPid();
    REQUIRE(child != 0);
    CHECK(gone(child));                             // agy's language server, which only the group's kill reaches
}

TEST_CASE("A finished status check leaves nothing of the group behind either", "[agy-signin]") {
    AcpRig rig({{u"child"_s, true}}, u"Google AI Pro"_s);
    KillOnExit cleanup(rig);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    REQUIRE(spies.status.count() == 1);
    REQUIRE(rig.childPid() != 0);
    CHECK(gone(rig.pid()));
    CHECK(gone(rig.childPid()));
}

TEST_CASE("Notifications and answers to other ids between the lines do not disturb the exchange", "[agy-signin]") {
    AcpRig rig({{u"interleave"_s, true}}, u"Google AI Pro"_s);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    REQUIRE(spies.status.count() == 1);
    CHECK(spies.failed.count() == 0);
    CHECK(spies.statusValue().state == SignInStatus::State::SignedIn);
    CHECK(spies.statusValue().detail == u"Google AI Pro"_s);
    QTest::qWait(100);
    CHECK(spies.ends() == 1);
}

TEST_CASE("An answer that arrives in two pieces is read as one line", "[agy-signin]") {
    AcpRig rig({{u"split"_s, true}}, u"Google AI Pro"_s);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    REQUIRE(spies.status.count() == 1);
    CHECK(spies.statusValue().state == SignInStatus::State::SignedIn);
    CHECK(spies.statusValue().detail == u"Google AI Pro"_s);
}

TEST_CASE("Megabytes without an end of line are a Parse failure, and agy is ended", "[agy-signin]") {
    AcpRig rig({{u"longLine"_s, u"session/new"_s}});
    KillOnExit cleanup(rig);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    CHECK(spies.status.count() == 0);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::Parse);
    CHECK(gone(rig.pid()));
}

TEST_CASE("The stream ending in the middle of an answer is a failure, never signed out", "[agy-signin]") {
    AcpRig rig({{u"truncate"_s, u"session/new"_s}});
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    CHECK(spies.status.count() == 0);
    REQUIRE(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::ConnectionRefused);
    QTest::qWait(100);
    CHECK(spies.ends() == 1);
}

TEST_CASE("A program and a folder with spaces in their paths work", "[agy-signin]") {
    AcpRig rig({}, u"Google AI Pro"_s);
    QTemporaryDir bin;
    REQUIRE(bin.isValid());
    const QString dir = bin.filePath(u"a folder/with spaces"_s);
    REQUIRE(QDir().mkpath(dir));
#ifdef Q_OS_WIN
    const QString program = dir + u"/agy.cmd"_s;
    REQUIRE(QFile::copy(rig.program(), program));
    REQUIRE(QFile::copy(QStringLiteral(NEXA_TESTS_DIR "/fake-agy/agy"), dir + u"/agy"_s));
#else
    const QString program = dir + u"/agy"_s;
    REQUIRE(QFile::copy(rig.program(), program));
#endif
    AgySignIn signIn(program, rig.dir.filePath(u"my work dir"_s));
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(spies.wait());
    REQUIRE(spies.status.count() == 1);
    CHECK(spies.statusValue().state == SignInStatus::State::SignedIn);
    CHECK(rig.starts() == 1);
}

TEST_CASE("A slot may start the next operation from the signal that ends one, once each", "[agy-signin]") {
    AcpRig rig;
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    int chained = 0;
    QObject::connect(&signIn, &ISignIn::statusChecked, &signIn, [&] {
        if (++chained == 1) signIn.checkStatus();
    });
    signIn.checkStatus();
    REQUIRE(spies.wait(2));
    QTest::qWait(200);
    CHECK(spies.status.count() == 2);
    CHECK(spies.failed.count() == 0);
    CHECK(rig.starts() == 2);
}

TEST_CASE("Cancelling while the session is being closed still ends once and ends agy", "[agy-signin]") {
    AcpRig rig({{u"hang"_s, u"session/close"_s}, {u"child"_s, true}}, u"Google AI Pro"_s);
    KillOnExit cleanup(rig);
    AgySignIn signIn(rig.program(), rig.workingDir());
    Spies spies(signIn);
    signIn.checkStatus();
    REQUIRE(logged(rig, u"session/close"_s));
    signIn.cancel();
    CHECK(spies.failed.count() == 1);
    CHECK(spies.error().code == ErrorCode::Cancelled);
    CHECK(gone(rig.pid()));
    CHECK(gone(rig.childPid()));
    QTest::qWait(300);
    CHECK(spies.ends() == 1);
}

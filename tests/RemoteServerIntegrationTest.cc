// remote/RemoteServer end to end: a real server on free ports, a fake Nexa behind it, and a client
// doing what the phone's page does — fetch the page, pair, open the WebSocket, ask, listen. Also the
// ways in that must stay shut: no token, a wrong token, a revoked token, another site's page.

#include "remote/IRemoteHost.h"
#include "remote/PairingManager.h"
#include "remote/RemoteServer.h"
#include "remote/RemoteSession.h"
#include "remote/TlsIdentity.h"

#include "QtStringMakers.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QSslError>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

class FakeHost : public IRemoteHost {
public:
    bool isBusy() const override { return busy; }
    void submit(const QString &text, const QString &source) override { submitted.append(source + u": "_s + text); }
    void cancel() override { ++cancels; }
    void transcribe(const QByteArray &pcm16k, std::function<void(Result<QString>)> done) override {
        heardBytes = pcm16k.size();
        done(u"open the calculator"_s);
    }
    bool canTranscribe() const override { return true; }
    void renderSpeech(const QString &text, std::function<void(Result<QByteArray>)> done) override {
        spoken = text;
        done(QByteArrayLiteral("RIFF-fake-wav"));
    }

    bool isLocked() const override { return locked; }
    QString lockText() const override { return locked ? u"Keyboard and mouse locked until “sort photos” is done."_s : QString(); }
    void unlock() override { ++unlocks; }
    QList<RemoteApproval> approvals() const override { return waiting; }
    void answer(quint64 id, bool allow) override { answers.append(u"%1:%2"_s.arg(id).arg(allow ? u"yes"_s : u"no"_s)); }

    bool busy{false};
    bool locked{false};
    int unlocks{0};
    QList<RemoteApproval> waiting;
    QStringList answers;
    QStringList submitted;
    int cancels{0};
    qsizetype heardBytes{0};
    QString spoken;
};

// Two free ports in a row: the server uses port and port + 1.
int freePortPair() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) continue;
        const int port = probe.serverPort();
        probe.close();
        QTcpServer first;
        QTcpServer second;
        if (port < 65000 && first.listen(QHostAddress::Any, quint16(port))
            && second.listen(QHostAddress::LocalHost, quint16(port + 1))) {
            return port;
        }
    }
    return 0;
}

// What the phone's page does with its WebSocket, recorded.
class Client {
public:
    explicit Client(const QString &origin) : socket(origin) {
        QObject::connect(&socket, &QWebSocket::textMessageReceived, [this](const QString &text) {
            messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
        });
        QObject::connect(&socket, &QWebSocket::sslErrors, [this](const QList<QSslError> &) { socket.ignoreSslErrors(); });
    }

    bool open(const QString &url) {
        QSignalSpy connected(&socket, &QWebSocket::connected);
        socket.open(QUrl(url));
        return connected.wait(5000);
    }

    void send(const QJsonObject &message) {
        socket.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }

    // The first message of `type` that arrives, waiting for it if need be.
    QJsonObject waitFor(const QString &type, int timeoutMs = 5000) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            for (qsizetype i = 0; i < messages.size(); ++i) {
                if (messages.at(i).value(u"t"_s).toString() == type) return messages.takeAt(i);
            }
            QTest::qWait(20);
        }
        return {};
    }

    bool waitForDisconnect(int timeoutMs = 5000) {
        if (socket.state() == QAbstractSocket::UnconnectedState) return true;
        QSignalSpy disconnected(&socket, &QWebSocket::disconnected);
        return disconnected.wait(timeoutMs);
    }

    QWebSocket socket;
    QList<QJsonObject> messages;
};

struct Fixture {
    Fixture() : pairing(directory.filePath(u"devices.json"_s)), tls(directory.filePath(u"tls"_s)),
                server(&pairing, &host, &tls) {}

    bool start() {
        port = freePortPair();
        if (port == 0) return false;
        bool done = false;
        server.start(port, [&done](Result<QString>) { done = true; });
        // The first start makes a certificate, which takes openssl a moment.
        return QTest::qWaitFor([&done] { return done; }, 30'000) && server.isRunning();
    }

    QString local(const QString &path) const { return u"http://127.0.0.1:%1%2"_s.arg(port + 1).arg(path); }
    QString lan(const QString &path) const { return u"https://127.0.0.1:%1%2"_s.arg(port).arg(path); }
    QString localOrigin() const { return u"http://127.0.0.1:%1"_s.arg(port + 1); }

    // POSTs to /api/pair and returns the status and the JSON answer.
    std::pair<int, QJsonObject> pair(const QString &code) {
        QNetworkRequest request(QUrl(local(u"/api/pair"_s)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
        QNetworkReply *reply = network.post(request, QJsonDocument(QJsonObject{{u"code"_s, code}, {u"name"_s, u"Test phone"_s}}).toJson());
        QSignalSpy finished(reply, &QNetworkReply::finished);
        finished.wait(5000);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        return {status, body};
    }

    std::pair<int, QByteArray> get(const QString &url, QByteArray *policy = nullptr) {
        QNetworkReply *reply = network.get(QNetworkRequest(QUrl(url)));
        QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) { reply->ignoreSslErrors(); });
        QSignalSpy finished(reply, &QNetworkReply::finished);
        finished.wait(5000);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (policy) *policy = reply->rawHeader("Content-Security-Policy");
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        return {status, body};
    }

    QString pairedToken() {
        pairing.startPairing();
        return pair(pairing.code()).second.value(u"token"_s).toString();
    }

    QTemporaryDir directory;
    PairingManager pairing;
    TlsIdentity tls;
    FakeHost host;
    RemoteServer server;
    QNetworkAccessManager network;
    int port{0};
};

bool haveOpenssl() {
    return !QStandardPaths::findExecutable(u"openssl"_s).isEmpty();
}

} // namespace

TEST_CASE("The page is served over HTTPS and over loopback, with a strict policy, and nothing else is") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    CHECK_FALSE(f.server.fingerprint().isEmpty());

    QByteArray policy;
    const auto [status, page] = f.get(f.local(u"/"_s), &policy);
    CHECK(status == 200);
    CHECK(page.contains("<title>Nexa</title>"));
    CHECK(policy.contains("default-src 'self'"));
    CHECK(f.get(f.lan(u"/"_s)).first == 200);
    CHECK(f.get(f.local(u"/app.js"_s)).first == 200);
    // Nexa's own icon, built in under the page's name for it.
    const auto [iconStatus, icon] = f.get(f.local(u"/icon.svg"_s));
    CHECK(iconStatus == 200);
    CHECK(icon.contains("<svg"));

    CHECK(f.get(f.local(u"/config.json"_s)).first == 404);
    CHECK(f.get(f.local(u"/..%2F..%2Fetc%2Fpasswd"_s)).first == 404);
    CHECK(f.get(f.local(u"/speech/0123456789abcdef.wav"_s)).first == 404);
}

TEST_CASE("Pairing hands out a token for the right code only") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());

    CHECK(f.pair(u"123456"_s).first == 403);                // no code is open
    f.pairing.startPairing();
    const QString code = f.pairing.code();
    CHECK(f.pair(code == u"000000" ? u"111111"_s : u"000000"_s).first == 403);
    const auto [status, body] = f.pair(code);
    CHECK(status == 200);
    CHECK(body.value(u"token"_s).toString().size() >= 43);
    CHECK(f.pair(code).first == 403);                       // spent
}

TEST_CASE("A paired phone asks, hears the conversation, and speaks") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    const QString token = f.pairedToken();
    REQUIRE_FALSE(token.isEmpty());

    Client phone(f.localOrigin());
    REQUIRE(phone.open(f.local(u"/ws"_s).replace(u"http:"_s, u"ws:"_s)));
    phone.send({{u"t"_s, u"hello"_s}, {u"token"_s, token}});
    const QJsonObject welcome = phone.waitFor(u"welcome"_s);
    CHECK(welcome.value(u"device"_s).toString() == u"Test phone"_s);
    CHECK(f.server.clientCount() == 1);

    // A typed request reaches Nexa as the phone's.
    phone.send({{u"t"_s, u"text"_s}, {u"text"_s, u"open firefox"_s}});
    CHECK(QTest::qWaitFor([&f] { return f.host.submitted == QStringList{u"phone: open firefox"_s}; }, 5000));

    // The conversation comes back, whoever started it.
    f.server.onUserMessage(u"volume 30"_s);
    CHECK(phone.waitFor(u"accepted"_s).value(u"source"_s).toString() == u"pc"_s);
    f.server.onReplyStarted(u"gui"_s);
    f.server.onReplyDelta(u"Done."_s);
    f.server.onReplyFinished(u"stop"_s);
    CHECK(phone.waitFor(u"delta"_s).value(u"text"_s).toString() == u"Done."_s);
    CHECK(phone.waitFor(u"finished"_s).value(u"reason"_s).toString() == u"stop"_s);

    // While Nexa is busy, a request is refused rather than lost.
    f.host.busy = true;
    phone.send({{u"t"_s, u"text"_s}, {u"text"_s, u"also this"_s}});
    CHECK(phone.waitFor(u"error"_s).value(u"code"_s).toString() == u"busy"_s);
    CHECK(f.host.submitted.size() == 1);
    f.host.busy = false;

    // A spoken request: transcribed, submitted as spoken, and answered aloud on the phone only.
    phone.socket.sendBinaryMessage(QByteArray(32'000, '\0'));
    CHECK(phone.waitFor(u"transcript"_s).value(u"text"_s).toString() == u"open the calculator"_s);
    CHECK(f.host.heardBytes == 32'000);
    CHECK(f.host.submitted.last() == u"phone-voice: open the calculator"_s);
    f.server.onReplyStarted(u"phone-voice"_s);
    f.server.onReplyStep(u"open_app — Opened Calculator · 9 ms"_s, true);
    f.server.onReplyFinished(u"stop"_s);
    const QString url = phone.waitFor(u"speech"_s).value(u"url"_s).toString();
    REQUIRE(url.startsWith(u"/speech/"_s));
    CHECK(f.host.spoken == u"Opened Calculator"_s);
    const auto [status, wav] = f.get(f.local(url));
    CHECK(status == 200);
    CHECK(wav == QByteArrayLiteral("RIFF-fake-wav"));

    // Cancel reaches Nexa.
    phone.send({{u"t"_s, u"cancel"_s}});
    CHECK(QTest::qWaitFor([&f] { return f.host.cancels == 1; }, 5000));

    // Too many requests at once are refused.
    for (int i = 0; i < RemoteSession::kMaxRequestsPerMinute + 2; ++i) {
        phone.send({{u"t"_s, u"text"_s}, {u"text"_s, u"again"_s}});
    }
    CHECK(phone.waitFor(u"error"_s).value(u"code"_s).toString() == u"too-fast"_s);
}

TEST_CASE("Without the right token, nothing gets through") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    const QString wsUrl = f.local(u"/ws"_s).replace(u"http:"_s, u"ws:"_s);

    Client stranger(f.localOrigin());
    REQUIRE(stranger.open(wsUrl));
    stranger.send({{u"t"_s, u"hello"_s}, {u"token"_s, u"made-up"_s}});
    CHECK(stranger.waitFor(u"error"_s).value(u"code"_s).toString() == u"unauthorized"_s);
    CHECK(stranger.waitForDisconnect());

    Client rude(f.localOrigin());
    REQUIRE(rude.open(wsUrl));
    rude.send({{u"t"_s, u"text"_s}, {u"text"_s, u"run rm -rf ~"_s}});
    CHECK(rude.waitForDisconnect());
    CHECK(f.host.submitted.isEmpty());

    // Another site's page in the phone's browser is turned away at the door.
    Client elsewhere(u"https://evil.example"_s);
    CHECK_FALSE(elsewhere.open(wsUrl));
}

TEST_CASE("Revoking a phone disconnects it at once") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    const QString token = f.pairedToken();
    Client phone(f.localOrigin());
    REQUIRE(phone.open(f.local(u"/ws"_s).replace(u"http:"_s, u"ws:"_s)));
    phone.send({{u"t"_s, u"hello"_s}, {u"token"_s, token}});
    REQUIRE_FALSE(phone.waitFor(u"welcome"_s).isEmpty());

    REQUIRE(f.pairing.revoke(f.pairing.devices().first().id));
    CHECK(phone.waitForDisconnect());

    Client again(f.localOrigin());
    REQUIRE(again.open(f.local(u"/ws"_s).replace(u"http:"_s, u"ws:"_s)));
    again.send({{u"t"_s, u"hello"_s}, {u"token"_s, token}});
    CHECK(again.waitFor(u"error"_s).value(u"code"_s).toString() == u"unauthorized"_s);
}

TEST_CASE("A locked PC shows on the phone, is unlocked from it, and its questions are answered there") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    const QString token = f.pairedToken();
    REQUIRE_FALSE(token.isEmpty());
    f.host.locked = true;
    f.host.waiting = {RemoteApproval{3, u"BROWSER-01"_s, u"browser_click"_s, u"Click “Submit”?"_s}};

    // A phone that connects while the PC is locked learns it at once, and what waits for an answer.
    Client phone(f.localOrigin());
    REQUIRE(phone.open(f.local(u"/ws"_s).replace(u"http:"_s, u"ws:"_s)));
    phone.send({{u"t"_s, u"hello"_s}, {u"token"_s, token}});
    REQUIRE_FALSE(phone.waitFor(u"welcome"_s).isEmpty());
    const QJsonObject lock = phone.waitFor(u"lock"_s);
    CHECK(lock.value(u"locked"_s).toBool());
    CHECK(lock.value(u"text"_s).toString().contains(u"sort photos"_s));
    const QJsonObject waiting = phone.waitFor(u"approval"_s);
    CHECK(waiting.value(u"id"_s).toInteger() == 3);
    CHECK(waiting.value(u"agent"_s).toString() == u"BROWSER-01"_s);

    phone.send({{u"t"_s, u"unlock"_s}});
    CHECK(QTest::qWaitFor([&f] { return f.host.unlocks == 1; }, 5000));
    f.server.onLockChanged(false, {});
    CHECK_FALSE(phone.waitFor(u"lock"_s).value(u"locked"_s).toBool(true));

    // A new question reaches every phone; the answer reaches Nexa; and every phone hears it was settled.
    f.server.onApprovalAsked(7, u"CODE-02"_s, u"run_command"_s, u"Run `make`?"_s);
    const QJsonObject asked = phone.waitFor(u"approval"_s);
    CHECK(asked.value(u"id"_s).toInteger() == 7);
    CHECK(asked.value(u"text"_s).toString() == u"Run `make`?"_s);
    phone.send({{u"t"_s, u"answer"_s}, {u"id"_s, 7}, {u"allow"_s, false}});
    CHECK(QTest::qWaitFor([&f] { return f.host.answers == QStringList{u"7:no"_s}; }, 5000));
    f.server.onApprovalSettled(7, false);
    CHECK(phone.waitFor(u"settled"_s).value(u"id"_s).toInteger() == 7);

    // An answer to nothing in particular is not guessed at.
    phone.send({{u"t"_s, u"answer"_s}, {u"allow"_s, true}});
    CHECK(phone.waitFor(u"error"_s).value(u"code"_s).toString() == u"invalid"_s);
    CHECK(f.host.answers.size() == 1);
}

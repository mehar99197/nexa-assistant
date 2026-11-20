// remote/RemoteClient — the phone app's end — against the real server: trust on first use, pairing,
// the conversation both ways, a spoken request, the spoken reply, cancel, being unpaired, and the
// doors that stay shut: a certificate that is not the trusted one, no pin at all.

#include "remote/IRemoteHost.h"
#include "remote/PairingManager.h"
#include "remote/RemoteClient.h"
#include "remote/RemoteRoutes.h"
#include "remote/RemoteServer.h"
#include "remote/TlsIdentity.h"

#include "QtStringMakers.h"

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <tuple>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

class FakeHost : public IRemoteHost {
public:
    bool isBusy() const override { return false; }
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

    QStringList submitted;
    int cancels{0};
    qsizetype heardBytes{0};
    QString spoken;
};

int freePortPair() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) continue;
        const int port = probe.serverPort();
        probe.close();
        QTcpServer first;
        QTcpServer second;
        QTcpServer third;
        if (port < 65000 && first.listen(QHostAddress::Any, quint16(port))
            && second.listen(QHostAddress::LocalHost, quint16(port + 1))
            && third.listen(QHostAddress::LocalHost, quint16(port + 2))) {
            return port;
        }
    }
    return 0;
}

struct Fixture {
    Fixture() : pairing(directory.filePath(u"devices.json"_s)), tls(directory.filePath(u"tls"_s)),
                server(&pairing, &host, &tls) {
        QObject::connect(&client, &RemoteClient::received, [this](const remote::ServerMessage &message) {
            received.append(message);
        });
    }

    bool start() {
        port = freePortPair();
        if (port == 0) return false;
        bool done = false;
        server.start(port, [&done](Result<QString>) { done = true; });
        return QTest::qWaitFor([&done] { return done; }, 30'000) && server.isRunning();
    }

    // The LAN side: TLS on `port`. The loopback host would mean plain HTTP, so it is set by hand.
    RemoteEndpoint lan() const { return RemoteEndpoint{u"127.0.0.1"_s, port, true, {}}; }
    RemoteEndpoint local() const { return RemoteEndpoint{u"127.0.0.1"_s, port + 1, false, {}}; }

    std::optional<Result<RemoteClient::Certificate>> probe() {
        std::optional<Result<RemoteClient::Certificate>> result;
        client.probe([&result](Result<RemoteClient::Certificate> r) { result = std::move(r); });
        std::ignore = QTest::qWaitFor([&result] { return result.has_value(); }, 10'000);
        return result;
    }

    std::optional<Result<QString>> pair(const QString &code) {
        std::optional<Result<QString>> result;
        client.pair(code, u"Nexa app on a test"_s, [&result](Result<QString> r) { result = std::move(r); });
        std::ignore = QTest::qWaitFor([&result] { return result.has_value(); }, 10'000);
        return result;
    }

    QString freshCode() {
        pairing.startPairing();
        return pairing.code();
    }

    // The first received message of `type`, waiting for it if need be.
    std::optional<remote::ServerMessage> waitFor(remote::ServerMessage::Type type, int timeoutMs = 5000) {
        std::optional<remote::ServerMessage> found;
        std::ignore = QTest::qWaitFor([this, type, &found] {
            for (qsizetype i = 0; i < received.size(); ++i) {
                if (received.at(i).type == type) {
                    found = received.takeAt(i);
                    return true;
                }
            }
            return false;
        }, timeoutMs);
        return found;
    }

    QTemporaryDir directory;
    PairingManager pairing;
    TlsIdentity tls;
    FakeHost host;
    RemoteServer server;
    RemoteClient client;
    QList<remote::ServerMessage> received;
    int port{0};
};

bool haveOpenssl() {
    return !QStandardPaths::findExecutable(u"openssl"_s).isEmpty();
}

} // namespace

TEST_CASE("An invitation is read in every form it comes in") {
    const auto plain = RemoteEndpoint::parse(u" 192.168.0.115:7777 "_s);
    REQUIRE(plain);
    CHECK(plain->host == u"192.168.0.115"_s);
    CHECK(plain->port == 7777);
    CHECK(plain->tls);
    CHECK(plain->code.isEmpty());
    CHECK(plain->url() == u"https://192.168.0.115:7777/"_s);
    CHECK(plain->url(u"/api/pair"_s) == u"https://192.168.0.115:7777/api/pair"_s);
    CHECK(plain->socketUrl() == u"wss://192.168.0.115:7777/ws"_s);

    const auto bare = RemoteEndpoint::parse(u"pc.local"_s);
    REQUIRE(bare);
    CHECK(bare->host == u"pc.local"_s);
    CHECK(bare->port == RemoteEndpoint::kDefaultPort);

    // What the PC's QR code holds, pasted whole: the code comes along.
    const auto invitation = RemoteEndpoint::parse(u"https://192.168.0.115:7777/#pair=123456"_s);
    REQUIRE(invitation);
    CHECK(invitation->host == u"192.168.0.115"_s);
    CHECK(invitation->port == 7777);
    CHECK(invitation->code == u"123456"_s);

    // Through adb reverse: the phone's own loopback, plain HTTP.
    const auto adb = RemoteEndpoint::parse(u"localhost:7777"_s);
    REQUIRE(adb);
    CHECK_FALSE(adb->tls);
    CHECK(adb->url() == u"http://localhost:7777/"_s);
    CHECK(adb->socketUrl() == u"ws://localhost:7777/ws"_s);
    const auto six = RemoteEndpoint::parse(u"[::1]:7777"_s);
    REQUIRE(six);
    CHECK(six->host == u"::1"_s);
    CHECK_FALSE(six->tls);

    CHECK_FALSE(RemoteEndpoint::parse(u""_s));
    CHECK_FALSE(RemoteEndpoint::parse(u"   "_s));
    CHECK_FALSE(RemoteEndpoint::parse(u"https://"_s));
    CHECK_FALSE(RemoteEndpoint::parse(u"not a host:port:junk"_s));
}

TEST_CASE("The phone app trusts Nexa's certificate once, pairs, talks and listens") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    f.client.setEndpoint(f.lan());

    // First contact: the certificate is shown, not trusted — it is the user who compares.
    const auto probed = f.probe();
    REQUIRE(probed);
    REQUIRE(probed->isOk());
    CHECK(probed->value().fingerprint == f.server.fingerprint());
    CHECK_FALSE(probed->value().trusted);

    // Without the pin nothing is sent, not even the pairing code.
    const QString code = f.freshCode();
    const auto refused = f.pair(code);
    REQUIRE(refused);
    CHECK_FALSE(refused->isOk());
    CHECK_FALSE(f.client.hasToken());
    CHECK(f.pairing.devices().isEmpty());

    f.client.setPinnedFingerprint(probed->value().fingerprint);
    const auto wrong = f.pair(code == u"000000" ? u"111111"_s : u"000000"_s);
    REQUIRE(wrong);
    CHECK_FALSE(wrong->isOk());
    const auto paired = f.pair(code);
    REQUIRE(paired);
    REQUIRE(paired->isOk());
    CHECK(f.client.hasToken());
    CHECK(f.client.token() == paired->value());

    QSignalSpy welcomed(&f.client, &RemoteClient::welcomed);
    f.client.connectToNexa();
    REQUIRE(welcomed.wait(5000));
    CHECK(welcomed.first().at(0).toString() == u"Nexa app on a test"_s);
    CHECK(welcomed.first().at(1).toBool());       // voice
    CHECK(f.client.isOnline());
    CHECK(f.server.clientCount() == 1);

    // A typed request reaches Nexa as the phone's.
    CHECK(f.client.ask(u"open firefox"_s));
    CHECK(QTest::qWaitFor([&f] { return f.host.submitted == QStringList{u"phone: open firefox"_s}; }, 5000));

    // The conversation comes back, whoever started it.
    f.server.onUserMessage(u"volume 30"_s);
    f.server.onReplyStarted(u"gui"_s);
    f.server.onReplyDelta(u"Done."_s);
    f.server.onReplyFinished(u"stop"_s);
    const auto accepted = f.waitFor(remote::ServerMessage::Type::Accepted);
    REQUIRE(accepted);
    CHECK(accepted->text == u"volume 30"_s);
    CHECK(accepted->source == u"pc"_s);
    CHECK(f.waitFor(remote::ServerMessage::Type::Delta)->text == u"Done."_s);
    CHECK(f.waitFor(remote::ServerMessage::Type::Finished)->reason == u"stop"_s);

    // A spoken request: transcribed, submitted as spoken, answered aloud — and the WAV fetched
    // under the same trust.
    CHECK(f.client.askAloud(QByteArray(32'000, '\0')));
    CHECK(f.waitFor(remote::ServerMessage::Type::Transcript)->text == u"open the calculator"_s);
    CHECK(f.host.heardBytes == 32'000);
    CHECK(f.host.submitted.last() == u"phone-voice: open the calculator"_s);
    f.server.onReplyStarted(u"phone-voice"_s);
    f.server.onReplyStep(u"open_app — Opened Calculator · 9 ms"_s, true);
    f.server.onReplyFinished(u"stop"_s);
    const auto speech = f.waitFor(remote::ServerMessage::Type::Speech);
    REQUIRE(speech);
    REQUIRE(speech->url.startsWith(u"/speech/"_s));
    std::optional<Result<QByteArray>> wav;
    f.client.fetch(speech->url, [&wav](Result<QByteArray> r) { wav = std::move(r); });
    REQUIRE(QTest::qWaitFor([&wav] { return wav.has_value(); }, 5000));
    REQUIRE(wav->isOk());
    CHECK(wav->value() == QByteArrayLiteral("RIFF-fake-wav"));
    CHECK_FALSE(f.client.askAloud(QByteArray(remote::kMaxAudioFrame + 2, '\0')));   // longer than a minute

    f.client.cancel();
    CHECK(QTest::qWaitFor([&f] { return f.host.cancels == 1; }, 5000));

    // Unpaired on the PC: the token is dropped and nothing tries again with it.
    QSignalSpy unauthorized(&f.client, &RemoteClient::unauthorized);
    REQUIRE(f.pairing.revoke(f.pairing.devices().first().id));
    REQUIRE(unauthorized.wait(5000));
    CHECK_FALSE(f.client.hasToken());
    CHECK(QTest::qWaitFor([&f] { return f.client.state() == RemoteClient::State::Offline; }, 5000));
    CHECK_FALSE(f.client.ask(u"anything"_s));
}

TEST_CASE("A certificate that is not the trusted one is refused before anything is sent") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    f.client.setEndpoint(f.lan());
    f.client.setPinnedFingerprint(u"AA:BB:CC"_s);
    f.client.setToken(u"some-token"_s);

    QSignalSpy rejected(&f.client, &RemoteClient::certificateRejected);
    QSignalSpy welcomed(&f.client, &RemoteClient::welcomed);
    f.client.connectToNexa();
    REQUIRE(rejected.wait(5000));
    CHECK(rejected.first().at(0).toString() == f.server.fingerprint());
    CHECK(QTest::qWaitFor([&f] { return f.client.state() == RemoteClient::State::Offline; }, 5000));
    CHECK(welcomed.isEmpty());
    CHECK(f.server.clientCount() == 0);
    // And it does not keep trying.
    QTest::qWait(300);
    CHECK(f.client.state() == RemoteClient::State::Offline);
}

TEST_CASE("Through adb reverse there is no certificate, and a lost server is waited for") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    f.client.setEndpoint(f.local());
    const auto probed = f.probe();
    REQUIRE(probed);
    REQUIRE(probed->isOk());
    CHECK(probed->value().trusted);
    CHECK(probed->value().fingerprint.isEmpty());

    const auto paired = f.pair(f.freshCode());
    REQUIRE(paired);
    REQUIRE(paired->isOk());
    QSignalSpy welcomed(&f.client, &RemoteClient::welcomed);
    f.client.connectToNexa();
    REQUIRE(welcomed.wait(5000));

    QSignalSpy states(&f.client, &RemoteClient::stateChanged);
    f.server.stop();
    CHECK(QTest::qWaitFor([&f] { return f.client.state() == RemoteClient::State::Waiting; }, 5000));
    CHECK(states.last().at(1).toString().contains(u"trying again"_s));
    f.client.disconnectFromNexa();
    CHECK(f.client.state() == RemoteClient::State::Offline);
}

namespace {

// A port where nothing answers: a phone away from home, knocking on the Wi-Fi address of a network it is not on.
int deadPort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) return 0;
    const int port = probe.serverPort();
    probe.close();
    return port;
}

} // namespace

TEST_CASE("Away from home the phone tries the next way, learns every way, and takes the PC's certificate") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    // Paired over adb (plain): no fingerprint pinned yet.
    f.client.setEndpoint(f.local());
    const auto paired = f.pair(f.freshCode());
    REQUIRE(paired);
    REQUIRE(paired->isOk());
    f.server.setRoutes({{RemoteRoute::Kind::Tailscale, u"https://127.0.0.1:%1"_s.arg(f.port)}});

    // The Wi-Fi it paired on does not answer; adb does.
    f.client.setEndpoint(RemoteEndpoint{u"127.0.0.1"_s, deadPort(), false, {}, u"Wi-Fi"_s});
    RemoteEndpoint adb = f.local();
    adb.via = u"adb"_s;
    f.client.setAlternates({adb});
    QSignalSpy learned(&f.client, &RemoteClient::routesLearned);
    QSignalSpy via(&f.client, &RemoteClient::connectedVia);
    f.client.connectToNexa();
    REQUIRE(via.wait(10'000));
    CHECK(via.last().at(0).toString() == u"adb"_s);
    REQUIRE_FALSE(learned.isEmpty());
    const QList<RemoteRoute> routes = learned.last().at(0).value<QList<RemoteRoute>>();
    REQUIRE(routes.size() == 1);
    CHECK(routes.first().kind == RemoteRoute::Kind::Tailscale);
    const QString fingerprint = learned.last().at(1).toString();
    CHECK(fingerprint == f.server.fingerprint());
    f.client.disconnectFromNexa();

    // With that certificate pinned, the direct way (Tailscale, Nexa's own HTTPS) is taken when the others fail.
    f.client.setPinnedFingerprint(fingerprint);
    f.client.setEndpoint(RemoteEndpoint{u"127.0.0.1"_s, deadPort(), false, {}, u"Wi-Fi"_s});
    const std::optional<RemoteEndpoint> tailscale = RemoteEndpoint::fromRoute(routes.first());
    REQUIRE(tailscale);
    CHECK(tailscale->via == u"Tailscale"_s);
    CHECK(tailscale->tls);
    CHECK_FALSE(tailscale->tunnel);
    f.client.setAlternates({*tailscale});
    f.client.connectToNexa();
    REQUIRE(via.wait(10'000));
    CHECK(via.last().at(0).toString() == u"Tailscale"_s);
    CHECK(f.client.via() == u"Tailscale"_s);
    f.client.disconnectFromNexa();
}

TEST_CASE("A tunnel's address showing Nexa's own certificate is refused, and the token is never sent to it") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.start());
    f.client.setEndpoint(f.local());
    const auto paired = f.pair(f.freshCode());
    REQUIRE(paired);
    REQUIRE(paired->isOk());
    // Pinned, as at home; but a "Cloudflare" way that answers with a self-signed certificate is not Cloudflare.
    f.client.setPinnedFingerprint(f.server.fingerprint());
    RemoteEndpoint impostor{u"127.0.0.1"_s, f.port, true, {}, u"Cloudflare"_s, true};
    f.client.setEndpoint(impostor);
    f.client.setAlternates({});
    QSignalSpy rejected(&f.client, &RemoteClient::certificateRejected);
    f.client.connectToNexa();
    REQUIRE(rejected.wait(10'000));
    QTest::qWait(500);
    CHECK(f.server.clientCount() == 0);
    CHECK(f.client.state() != RemoteClient::State::Online);
    f.client.disconnectFromNexa();
}

TEST_CASE("A tunnel's web address has the web's port, Nexa's own address has Nexa's") {
    const auto cloudflare = RemoteEndpoint::fromRoute({RemoteRoute::Kind::Cloudflare, u"https://nexa.nexacodesolutions.online"_s});
    REQUIRE(cloudflare);
    CHECK(cloudflare->port == 443);
    CHECK(cloudflare->tunnel);
    CHECK(cloudflare->socketUrl() == u"wss://nexa.nexacodesolutions.online:443/ws"_s);
    CHECK(RemoteEndpoint::parse(u"pc.local"_s)->port == RemoteEndpoint::kDefaultPort);
    const auto adb = RemoteEndpoint::fromRoute({RemoteRoute::Kind::Adb, u"http://localhost:7777"_s});
    REQUIRE(adb);
    CHECK_FALSE(adb->tls);
    CHECK(adb->via == u"adb"_s);
}

// system/net/TunnelProcess: a tunnel program run, its log read. /bin/sh stands in for cloudflared and ngrok, printing
// their real lines, so no test reaches the internet.

#include "system/net/TunnelProcess.h"

#include "QtStringMakers.h"

#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using State = TunnelProcess::State;

namespace {

TunnelProcess::Spec saying(const QString &script) {
    return TunnelProcess::Spec{u"/bin/sh"_s, {u"-c"_s, script}, &tunnellog::cloudflaredLine, {}};
}

} // namespace

TEST_CASE("A tunnel that comes up gives its address, and goes when stopped") {
    TunnelProcess tunnel(u"cloudflared"_s);
    tunnel.start(saying(u"echo 'INF |  https://shaped-oak.trycloudflare.com  |'; "
                        "echo 'INF Registered tunnel connection connIndex=0 protocol=quic'; sleep 30"_s));
    REQUIRE(QTest::qWaitFor([&tunnel] { return tunnel.state() == State::Running; }, 5000));
    CHECK(tunnel.url() == u"https://shaped-oak.trycloudflare.com"_s);
    CHECK(tunnel.problem().isEmpty());
    tunnel.stop();
    CHECK(tunnel.state() == State::Off);
    CHECK(tunnel.url().isEmpty());
}

TEST_CASE("A tunnel that only the owner can fix says why and is not started again") {
    TunnelProcess tunnel(u"cloudflared"_s);
    tunnel.start(saying(u"echo 'ERR Cannot determine default origin certificate path. No file cert.pem in "
                        "[~/.cloudflared]'; sleep 30"_s));
    REQUIRE(QTest::qWaitFor([&tunnel] { return tunnel.state() == State::Failed; }, 5000));
    CHECK(tunnel.problem().contains(u"cert.pem"_s));
    QTest::qWait(300);
    CHECK(tunnel.state() == State::Failed);     // no restart loop
}

TEST_CASE("A named tunnel's address is known before it connects; one not installed says where to get it") {
    TunnelProcess named(u"cloudflared"_s);
    TunnelProcess::Spec spec = saying(u"sleep 1; echo 'INF Registered tunnel connection connIndex=0'; sleep 30"_s);
    spec.fixedUrl = u"https://nexa.nexacodesolutions.online"_s;
    named.start(spec);
    CHECK(named.state() == State::Starting);
    CHECK(named.url().isEmpty());       // not before it is reachable
    REQUIRE(QTest::qWaitFor([&named] { return named.state() == State::Running; }, 5000));
    CHECK(named.url() == u"https://nexa.nexacodesolutions.online"_s);

    TunnelProcess missing(u"ngrok"_s);
    missing.start(TunnelProcess::Spec{{}, {}, &tunnellog::ngrokLine, {}});
    CHECK(missing.state() == State::Missing);
    CHECK(missing.problem().contains(u"download-tunnels.sh"_s));
}

TEST_CASE("A tunnel that stops by itself is started again") {
    TunnelProcess tunnel(u"ngrok"_s);
    tunnel.start(TunnelProcess::Spec{u"/bin/sh"_s, {u"-c"_s, u"exit 3"_s}, &tunnellog::ngrokLine, {}});
    REQUIRE(QTest::qWaitFor([&tunnel] { return tunnel.problem().contains(u"stopped"_s); }, 5000));
    CHECK(tunnel.state() == State::Starting);       // waiting to try again
    tunnel.stop();
    CHECK(tunnel.state() == State::Off);
}

TEST_CASE("Where the network's DNS cannot find cloudflared's servers, it is started again naming them") {
    // Without --edge it says what cloudflared says on such a router and exits; with it, it connects.
    const QString script = u"case \"$*\" in *--edge*) echo 'INF |  https://named-edge.trycloudflare.com  |'; "
                           "echo 'INF Registered tunnel connection connIndex=0'; sleep 30;; "
                           "*) echo 'ERR Initiating shutdown error=\"Could not lookup srv records on "
                           "_v2-origintunneld._tcp.argotunnel.com: cannot unmarshal DNS message\"'; exit 1;; esac"_s;
    TunnelProcess tunnel(u"cloudflared"_s);
    tunnel.start(TunnelProcess::Spec{u"/bin/sh"_s, {u"-c"_s, script, u"sh"_s}, &tunnellog::cloudflaredLine, {},
                                     {u"-c"_s, script, u"sh"_s, u"--edge"_s}});
    REQUIRE(QTest::qWaitFor([&tunnel] { return tunnel.state() == State::Running; }, 5000));
    CHECK(tunnel.url() == u"https://named-edge.trycloudflare.com"_s);
}

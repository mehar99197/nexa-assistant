// system/net/TunnelProcess: a tunnel program run, its log read. /bin/sh stands in for cloudflared and ngrok, printing
// their real lines, so no test reaches the internet.

#include "system/net/TunnelProcess.h"

#include "QtStringMakers.h"

#include <QStandardPaths>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using State = TunnelProcess::State;

namespace {

#ifdef Q_OS_WIN
QString pythonExe() {
    if (const QString p = QStandardPaths::findExecutable(u"python3"_s); !p.isEmpty()) return p;
    return QStandardPaths::findExecutable(u"python"_s);
}

TunnelProcess::Spec sayingPy(const QString &pyScript) {
    return TunnelProcess::Spec{pythonExe(), {u"-u"_s, u"-c"_s, pyScript}, &tunnellog::cloudflaredLine, {}};
}
#endif

TunnelProcess::Spec saying(const QString &script, const QString &pyScript) {
#ifdef Q_OS_WIN
    Q_UNUSED(script);
    return sayingPy(pyScript);
#else
    Q_UNUSED(pyScript);
    return TunnelProcess::Spec{u"/bin/sh"_s, {u"-c"_s, script}, &tunnellog::cloudflaredLine, {}};
#endif
}

} // namespace

TEST_CASE("A tunnel that comes up gives its address, and goes when stopped") {
    TunnelProcess tunnel(u"cloudflared"_s);
    tunnel.start(saying(
        u"echo 'INF |  https://shaped-oak.trycloudflare.com  |'; "
        "echo 'INF Registered tunnel connection connIndex=0 protocol=quic'; sleep 30"_s,
        u"import time; print('INF |  https://shaped-oak.trycloudflare.com  |', flush=True); "
        "print('INF Registered tunnel connection connIndex=0 protocol=quic', flush=True); time.sleep(30)"_s));
    REQUIRE(QTest::qWaitFor([&tunnel] { return tunnel.state() == State::Running; }, 5000));
    CHECK(tunnel.url() == u"https://shaped-oak.trycloudflare.com"_s);
    CHECK(tunnel.problem().isEmpty());
    tunnel.stop();
    CHECK(tunnel.state() == State::Off);
    CHECK(tunnel.url().isEmpty());
}

TEST_CASE("A tunnel that only the owner can fix says why and is not started again") {
    TunnelProcess tunnel(u"cloudflared"_s);
    tunnel.start(saying(
        u"echo 'ERR Cannot determine default origin certificate path. No file cert.pem in "
        "[~/.cloudflared]'; sleep 30"_s,
        u"import time; print('ERR Cannot determine default origin certificate path. No file cert.pem in "
        "[~/.cloudflared]', flush=True); time.sleep(30)"_s));
    REQUIRE(QTest::qWaitFor([&tunnel] { return tunnel.state() == State::Failed; }, 5000));
    CHECK(tunnel.problem().contains(u"cert.pem"_s));
    QTest::qWait(300);
    CHECK(tunnel.state() == State::Failed);     // no restart loop
}

TEST_CASE("A named tunnel's address is known before it connects; one not installed says where to get it") {
    TunnelProcess named(u"cloudflared"_s);
    TunnelProcess::Spec spec = saying(
        u"sleep 1; echo 'INF Registered tunnel connection connIndex=0'; sleep 30"_s,
        u"import time; time.sleep(1); print('INF Registered tunnel connection connIndex=0', flush=True); time.sleep(30)"_s);
    spec.fixedUrl = u"https://nexa.nexacodesolutions.online"_s;
    named.start(spec);
    CHECK(named.state() == State::Starting);
    CHECK(named.url().isEmpty());       // not before it is reachable
    REQUIRE(QTest::qWaitFor([&named] { return named.state() == State::Running; }, 5000));
    CHECK(named.url() == u"https://nexa.nexacodesolutions.online"_s);

    TunnelProcess missing(u"ngrok"_s);
    missing.start(TunnelProcess::Spec{{}, {}, &tunnellog::ngrokLine, {}});
    CHECK(missing.state() == State::Missing);
#ifdef Q_OS_WIN
    CHECK(missing.problem().contains(u"LOCALAPPDATA"_s));
#else
    CHECK(missing.problem().contains(u"download-tunnels.sh"_s));
#endif
}

TEST_CASE("A tunnel that stops by itself is started again") {
    TunnelProcess tunnel(u"ngrok"_s);
#ifdef Q_OS_WIN
    tunnel.start(TunnelProcess::Spec{pythonExe(), {u"-u"_s, u"-c"_s, u"import sys; sys.exit(3)"_s},
                                     &tunnellog::ngrokLine, {}});
#else
    tunnel.start(TunnelProcess::Spec{u"/bin/sh"_s, {u"-c"_s, u"exit 3"_s}, &tunnellog::ngrokLine, {}});
#endif
    REQUIRE(QTest::qWaitFor([&tunnel] { return tunnel.problem().contains(u"stopped"_s); }, 5000));
    CHECK(tunnel.state() == State::Starting);       // waiting to try again
    tunnel.stop();
    CHECK(tunnel.state() == State::Off);
}

TEST_CASE("Where the network's DNS cannot find cloudflared's servers, it is started again naming them") {
    // Without --edge it says what cloudflared says on such a router and exits; with it, it connects.
    TunnelProcess tunnel(u"cloudflared"_s);
#ifdef Q_OS_WIN
    const QString script = u"import sys, time\n"
                           "if '--edge' in sys.argv:\n"
                           "  print('INF |  https://named-edge.trycloudflare.com  |', flush=True)\n"
                           "  print('INF Registered tunnel connection connIndex=0', flush=True)\n"
                           "  time.sleep(30)\n"
                           "else:\n"
                           "  print('ERR Initiating shutdown error=\"Could not lookup srv records on "
                           "_v2-origintunneld._tcp.argotunnel.com: cannot unmarshal DNS message\"', flush=True)\n"
                           "  sys.exit(1)\n"_s;
    tunnel.start(TunnelProcess::Spec{pythonExe(), {u"-u"_s, u"-c"_s, script}, &tunnellog::cloudflaredLine, {},
                                     {u"-u"_s, u"-c"_s, script, u"--edge"_s}});
#else
    const QString script = u"case \"$*\" in *--edge*) echo 'INF |  https://named-edge.trycloudflare.com  |'; "
                           "echo 'INF Registered tunnel connection connIndex=0'; sleep 30;; "
                           "*) echo 'ERR Initiating shutdown error=\"Could not lookup srv records on "
                           "_v2-origintunneld._tcp.argotunnel.com: cannot unmarshal DNS message\"'; exit 1;; esac"_s;
    tunnel.start(TunnelProcess::Spec{u"/bin/sh"_s, {u"-c"_s, script, u"sh"_s}, &tunnellog::cloudflaredLine, {},
                                     {u"-c"_s, script, u"sh"_s, u"--edge"_s}});
#endif
    REQUIRE(QTest::qWaitFor([&tunnel] { return tunnel.state() == State::Running; }, 5000));
    CHECK(tunnel.url() == u"https://named-edge.trycloudflare.com"_s);
}

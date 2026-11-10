// system/net/TunnelLog: what cloudflared, ngrok and Tailscale say, read into states and addresses. The lines are the
// programs' own (cloudflared 2025.x, ngrok 3.x with --log-format json, tailscale status --json).

#include "system/net/TunnelLog.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::tunnellog;
using namespace Qt::StringLiterals;

TEST_CASE("cloudflared: a quick tunnel's address, the connection, and its errors") {
    CHECK(cloudflaredLine(u"2026-11-11T01:30:02Z INF |  https://shaped-carefully-guarantee-oak.trycloudflare.com     |"_s).url
          == u"https://shaped-carefully-guarantee-oak.trycloudflare.com"_s);
    const LineEvent registered = cloudflaredLine(
        u"2026-11-11T01:30:03Z INF Registered tunnel connection connIndex=0 connection=8c2e event=0 ip=198.41.200.13 "
        "location=fra08 protocol=quic"_s);
    CHECK(registered.connected);
    CHECK(registered.url.isEmpty());
    const LineEvent noCert = cloudflaredLine(
        u"2026-11-11T01:30:00Z ERR Cannot determine default origin certificate path. No file cert.pem in "
        "[~/.cloudflared ~/.cloudflare-warp ~/cloudflare-warp /etc/cloudflared /usr/local/etc/cloudflared]"_s);
    CHECK(noCert.error.contains(u"cert.pem"_s));
    CHECK(noCert.fatal);
    // A passing complaint is not a failure.
    const LineEvent features = cloudflaredLine(
        u"2026-11-11T01:30:00Z ERR Failed to fetch features, default to disable error=\"lookup cfd-features.argotunnel.com\""_s);
    CHECK_FALSE(features.fatal);
    CHECK(cloudflaredLine(u"2026-11-11T01:30:00Z INF Starting tunnel tunnelID=6ff42ae2"_s).error.isEmpty());
}

TEST_CASE("ngrok: the tunnel's address, and why it would not start") {
    const LineEvent started = ngrokLine(
        uR"({"addr":"http://127.0.0.1:7779","lvl":"info","msg":"started tunnel","name":"command_line","obj":"tunnels","t":"2026-11-11T01:31:00Z","url":"https://quiet-owl.ngrok-free.app"})"_s);
    CHECK(started.url == u"https://quiet-owl.ngrok-free.app"_s);
    CHECK(started.connected);
    const LineEvent noToken = ngrokLine(
        uR"({"err":"authentication failed: Usage of ngrok requires a verified account and authtoken.\n\nSign up for an account: https://dashboard.ngrok.com/signup\r\n\r\nERR_NGROK_4018\r\n","lvl":"eror","msg":"failed to reconnect session","obj":"tunnels.session","t":"2026-11-11T01:31:00Z"})"_s);
    CHECK(noToken.fatal);
    CHECK(noToken.error.startsWith(u"authentication failed: Usage of ngrok requires a verified account and authtoken."_s));
    CHECK(noToken.error.contains(u"ERR_NGROK_4018"_s));
    CHECK_FALSE(noToken.error.contains(u'\n'));
    // Not JSON (its banner), or a line of no interest: nothing.
    CHECK(ngrokLine(u"t=2026 lvl=info msg=\"no configuration paths supplied\""_s).url.isEmpty());
    CHECK(ngrokLine(uR"({"lvl":"info","msg":"client session established","obj":"tunnels.session"})"_s).error.isEmpty());
}

TEST_CASE("tailscale status --json: signed in with an address, signed out, or no daemon") {
    const TailscaleState running = parseTailscaleStatus(QByteArrayLiteral(
        R"({"BackendState":"Running","MagicDNSSuffix":"tail1234.ts.net","Self":{"HostName":"nexa-laptop","DNSName":"nexa-laptop.tail1234.ts.net.","TailscaleIPs":["100.101.102.103","fd7a:115c:a1e0::1"],"Online":true}})"));
    CHECK(running.signedIn);
    CHECK(running.ipv4 == u"100.101.102.103"_s);
    CHECK(running.dnsName == u"nexa-laptop.tail1234.ts.net"_s);
    const TailscaleState out = parseTailscaleStatus(QByteArrayLiteral(
        R"({"BackendState":"NeedsLogin","AuthURL":"https://login.tailscale.com/a/abc","Self":{"TailscaleIPs":null}})"));
    CHECK_FALSE(out.signedIn);
    CHECK(out.backend == u"NeedsLogin"_s);
    CHECK(out.ipv4.isEmpty());
    const TailscaleState none = parseTailscaleStatus(QByteArrayLiteral("failed to connect to local tailscaled; it doesn't appear to be running"));
    CHECK_FALSE(none.signedIn);
    CHECK(none.backend.isEmpty());
    CHECK(tailscaleLoginUrl(u"To authenticate, visit:\n\n\thttps://login.tailscale.com/a/gcab8eaf0d94652e0\n"_s)
          == u"https://login.tailscale.com/a/gcab8eaf0d94652e0"_s);
    CHECK(tailscaleLoginUrl(u"Success."_s).isEmpty());
}

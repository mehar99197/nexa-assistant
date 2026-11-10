// remote/RemoteRoutes: the ways a phone can reach this PC — adb, the Wi-Fi, Tailscale, Cloudflare, ngrok — in the
// order a phone tries them, and their wire form, read back strictly (the other end is a network).

#include "remote/RemoteRoutes.h"

#include "QtStringMakers.h"

#include <QJsonArray>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using Kind = RemoteRoute::Kind;

TEST_CASE("Routes are tried nearest first: adb, the Wi-Fi, Tailscale, then the tunnels") {
    const QList<RemoteRoute> mixed{
        {Kind::Ngrok, u"https://quiet-owl.ngrok-free.app"_s},
        {Kind::Cloudflare, u"https://nexa.nexacodesolutions.online"_s},
        {Kind::Tailscale, u"https://100.101.102.103:7777"_s},
        {Kind::Lan, u"https://192.168.0.109:7777"_s},
        {Kind::Adb, u"http://localhost:7777"_s},
    };
    const QList<RemoteRoute> ordered = orderedRoutes(mixed);
    REQUIRE(ordered.size() == 5);
    CHECK(ordered.at(0).kind == Kind::Adb);
    CHECK(ordered.at(1).kind == Kind::Lan);
    CHECK(ordered.at(2).kind == Kind::Tailscale);
    CHECK(ordered.at(3).kind == Kind::Cloudflare);
    CHECK(ordered.at(4).kind == Kind::Ngrok);
    // The same way twice is one way.
    CHECK(orderedRoutes({mixed.at(0), mixed.at(0)}).size() == 1);
}

TEST_CASE("A route says whether Nexa's own certificate answers it") {
    CHECK(RemoteRoute{Kind::Lan, u"https://192.168.0.109:7777"_s}.ownCertificate());
    CHECK(RemoteRoute{Kind::Tailscale, u"https://100.101.102.103:7777"_s}.ownCertificate());
    CHECK_FALSE(RemoteRoute{Kind::Cloudflare, u"https://nexa.nexacodesolutions.online"_s}.ownCertificate());
    CHECK_FALSE(RemoteRoute{Kind::Ngrok, u"https://quiet-owl.ngrok-free.app"_s}.ownCertificate());
    CHECK_FALSE(RemoteRoute{Kind::Adb, u"http://localhost:7777"_s}.ownCertificate());     // plain, on the phone itself
    CHECK(routeName(Kind::Tailscale) == u"Tailscale"_s);
    CHECK(routeName(Kind::Lan) == u"Wi-Fi"_s);
}

TEST_CASE("Routes go over the wire and come back exactly; anything doubtful is left out") {
    const QList<RemoteRoute> routes{
        {Kind::Lan, u"https://192.168.0.109:7777"_s},
        {Kind::Tailscale, u"https://nexa-laptop.tail1234.ts.net:7777"_s},
        {Kind::Cloudflare, u"https://nexa.nexacodesolutions.online"_s},
    };
    CHECK(decodeRoutes(encodeRoutes(routes)) == routes);

    const QJsonArray doubtful{
        QJsonObject{{u"kind"_s, u"lan"_s}, {u"url"_s, u"https://192.168.0.109:7777"_s}},     // kept
        QJsonObject{{u"kind"_s, u"teleport"_s}, {u"url"_s, u"https://x.example"_s}},          // unknown kind
        QJsonObject{{u"kind"_s, u"cloudflare"_s}, {u"url"_s, u"http://nexa.example.com"_s}},  // a tunnel without TLS
        QJsonObject{{u"kind"_s, u"lan"_s}, {u"url"_s, u"javascript:alert(1)"_s}},            // not a web address
        QJsonObject{{u"kind"_s, u"adb"_s}, {u"url"_s, u"http://192.168.0.5:7777"_s}},        // adb is the phone itself
        QJsonObject{{u"kind"_s, u"ngrok"_s}},                                                  // no address
        QJsonValue(u"https://loose.example"_s),
    };
    const QList<RemoteRoute> kept = decodeRoutes(doubtful);
    REQUIRE(kept.size() == 1);
    CHECK(kept.first().kind == Kind::Lan);
}

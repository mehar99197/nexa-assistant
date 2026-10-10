// remote/RemoteServer::sameOrigin: only the page Nexa served may open its WebSocket. Another site
// open in the phone's browser must not be able to drive the PC through it.

#include "remote/RemoteServer.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("The page Nexa served may open the WebSocket") {
    CHECK(RemoteServer::sameOrigin(u"https://192.168.0.115:7777"_s, u"192.168.0.115:7777"_s));
    // Through adb reverse the phone still says localhost:7777, whichever port the PC uses.
    CHECK(RemoteServer::sameOrigin(u"http://localhost:7777"_s, u"localhost:7777"_s));
    CHECK(RemoteServer::sameOrigin(u"https://MyPC.local:7777"_s, u"mypc.local:7777"_s));
    CHECK(RemoteServer::sameOrigin(u"https://[fe80::1]:7777"_s, u"[fe80::1]:7777"_s));
    // Default ports may be left out on either side.
    CHECK(RemoteServer::sameOrigin(u"https://nexa.example"_s, u"nexa.example:443"_s));
    CHECK(RemoteServer::sameOrigin(u"http://nexa.example:80"_s, u"nexa.example"_s));
}

TEST_CASE("Any other site may not") {
    CHECK_FALSE(RemoteServer::sameOrigin(u"https://evil.example"_s, u"192.168.0.115:7777"_s));
    CHECK_FALSE(RemoteServer::sameOrigin(u"https://192.168.0.115:8443"_s, u"192.168.0.115:7777"_s));
    CHECK_FALSE(RemoteServer::sameOrigin(u"https://192.168.0.115.evil.example:7777"_s, u"192.168.0.115:7777"_s));
    CHECK_FALSE(RemoteServer::sameOrigin(u"null"_s, u"192.168.0.115:7777"_s));            // a sandboxed frame
    CHECK_FALSE(RemoteServer::sameOrigin(u"file:///sdcard/page.html"_s, u"localhost:7777"_s));
    CHECK_FALSE(RemoteServer::sameOrigin(u"https://[fe80::1]:7777"_s, u"[fe80::1:7777"_s));  // broken Host
}

TEST_CASE("A client that is not a page sends no Origin, and still needs a token") {
    CHECK(RemoteServer::sameOrigin(QString(), u"192.168.0.115:7777"_s));
}

// core/ai/OpenCodeWire: opencode's server API, with payloads as opencode 1.19.3 sent them on 2026-10-27;
// and HttpClient::getStreaming, which reads its event stream.

#include "core/ai/OpenCodeWire.h"
#include "core/net/HttpClient.h"

#include "FakeHttpServer.h"
#include "QtStringMakers.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::test;
using namespace Qt::StringLiterals;

TEST_CASE("promptBody carries Nexa's system prompt with opencode's tools off") {
    const QJsonObject body = OpenCodeWire::promptBody(u"google/antigravity-gemini-3-flash"_s, u"S"_s, u"hi"_s, {});
    CHECK(body.value(u"model"_s).toObject() == QJsonObject{{u"providerID"_s, u"google"_s},
                                                            {u"modelID"_s, u"antigravity-gemini-3-flash"_s}});
    CHECK(body.value(u"system"_s).toString() == u"S"_s);
    CHECK(body.value(u"tools"_s).toObject() == QJsonObject{{u"*"_s, false}});
    const QJsonArray parts = body.value(u"parts"_s).toArray();
    REQUIRE(parts.size() == 1);
    CHECK(parts.at(0).toObject() == QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, u"hi"_s}});

    // Only the first slash separates provider and model.
    CHECK(OpenCodeWire::promptBody(u"openrouter/meta/llama"_s, {}, u"x"_s, {})
              .value(u"model"_s).toObject().value(u"modelID"_s).toString()
          == u"meta/llama"_s);
    // No model: opencode's own default; no system prompt: none sent.
    const QJsonObject bare = OpenCodeWire::promptBody({}, {}, u"x"_s, {});
    CHECK_FALSE(bare.contains(u"model"_s));
    CHECK_FALSE(bare.contains(u"system"_s));

    const QJsonArray withImage =
        OpenCodeWire::promptBody({}, {}, u"look"_s, {ImageAttachment{"PNGDATA", u"image/png"_s}})
            .value(u"parts"_s).toArray();
    REQUIRE(withImage.size() == 2);
    const QJsonObject file = withImage.at(1).toObject();
    CHECK(file.value(u"type"_s).toString() == u"file"_s);
    CHECK(file.value(u"mime"_s).toString() == u"image/png"_s);
    CHECK(file.value(u"url"_s).toString() == u"data:image/png;base64,"_s + QString::fromLatin1(QByteArray("PNGDATA").toBase64()));
}

TEST_CASE("decodeEvent reads deltas, status, idle and errors") {
    const OpenCodeEvent delta = OpenCodeWire::decodeEvent(
        R"({"id":"evt_1","type":"message.part.delta","properties":{"sessionID":"ses_1","messageID":"msg_1","partID":"prt_1","field":"text","delta":"Hello! Mars is one planet."}})");
    CHECK(delta.kind == OpenCodeEvent::Kind::TextDelta);
    CHECK(delta.sessionId == u"ses_1"_s);
    CHECK(delta.text == u"Hello! Mars is one planet."_s);

    CHECK(OpenCodeWire::decodeEvent(
              R"({"type":"message.part.delta","properties":{"sessionID":"ses_1","messageID":"m","partID":"p","field":"reasoning","delta":"hmm"}})")
              .kind
          == OpenCodeEvent::Kind::Other);

    const OpenCodeEvent busy = OpenCodeWire::decodeEvent(
        R"({"type":"session.status","properties":{"sessionID":"ses_1","status":{"type":"busy"}}})");
    CHECK(busy.kind == OpenCodeEvent::Kind::Busy);

    const OpenCodeEvent retry = OpenCodeWire::decodeEvent(
        R"({"type":"session.status","properties":{"sessionID":"ses_1","status":{"type":"retry","attempt":1,"message":"Free usage exceeded, subscribe to Go","action":{"reason":"free_tier_limit","provider":"opencode","title":"Free limit reached"}}}})");
    CHECK(retry.kind == OpenCodeEvent::Kind::Retry);
    CHECK(retry.message == u"Free usage exceeded, subscribe to Go"_s);
    CHECK(retry.reason == u"free_tier_limit"_s);

    const OpenCodeEvent idle = OpenCodeWire::decodeEvent(R"({"type":"session.idle","properties":{"sessionID":"ses_1"}})");
    CHECK(idle.kind == OpenCodeEvent::Kind::Idle);
    CHECK(idle.sessionId == u"ses_1"_s);

    const OpenCodeEvent auth = OpenCodeWire::decodeEvent(
        R"({"type":"session.error","properties":{"sessionID":"ses_1","error":{"name":"ProviderAuthError","data":{"providerID":"google","message":"x"}}}})");
    CHECK(auth.kind == OpenCodeEvent::Kind::Error);
    CHECK(auth.authError);
    CHECK(auth.message == u"x"_s);

    const OpenCodeEvent other = OpenCodeWire::decodeEvent(
        R"({"type":"session.error","properties":{"sessionID":"ses_1","error":{"name":"APIError","data":{"message":"overloaded"}}}})");
    CHECK_FALSE(other.authError);
    CHECK(other.message == u"overloaded"_s);

    CHECK(OpenCodeWire::decodeEvent(R"({"type":"server.heartbeat","properties":{}})").kind == OpenCodeEvent::Kind::Other);
    CHECK(OpenCodeWire::decodeEvent("not json").kind == OpenCodeEvent::Kind::Other);
}

TEST_CASE("parseModels lists every provider's models with the default first") {
    const QByteArray providers =
        R"({"providers":[{"id":"other","models":{"one":{}}},{"id":"fake","models":{"smart":{},"fast":{}}}],"default":{"fake":"smart"}})";
    CHECK(OpenCodeWire::parseModels(providers, u"fake/smart"_s)
          == QStringList{u"fake/smart"_s, u"fake/fast"_s, u"other/one"_s});
    CHECK(OpenCodeWire::parseModels(providers, u"missing/model"_s)
          == QStringList{u"fake/fast"_s, u"fake/smart"_s, u"other/one"_s});
    CHECK(OpenCodeWire::parseDefaultModel(R"({"model":"google/antigravity-gemini-3-flash","lsp":true})")
          == u"google/antigravity-gemini-3-flash"_s);
    CHECK(OpenCodeWire::parseDefaultModel(R"({"lsp":true})").isEmpty());
}

TEST_CASE("parseListening finds the server's address") {
    CHECK(OpenCodeWire::parseListening("opencode server listening on http://127.0.0.1:4096\n")
          == QUrl(u"http://127.0.0.1:4096"_s));
    CHECK_FALSE(OpenCodeWire::parseListening("[auto-update-checker] Package not found\n").isValid());
    CHECK(OpenCodeWire::parseSessionId(R"({"id":"ses_e82111ec8ffe","title":"Nexa"})") == u"ses_e82111ec8ffe"_s);
}

TEST_CASE("request authenticates as opencode") {
    const QNetworkRequest request =
        OpenCodeWire::request(QUrl(u"http://127.0.0.1:4096"_s), u"/session"_s, u"pw"_s);
    CHECK(request.url() == QUrl(u"http://127.0.0.1:4096/session"_s));
    CHECK(request.rawHeader("Authorization") == "Basic " + QByteArray("opencode:pw").toBase64());
    CHECK(request.header(QNetworkRequest::ContentTypeHeader).toString() == u"application/json"_s);
}

TEST_CASE("getStreaming reads an event stream as it arrives") {
    FakeHttpServer server;
    server.respond({.chunks = {"data: {\"a\":1}\n\n", "data: {\"a\":2}\n\n"}, .holdOpen = true});
    HttpClient http;
    QNetworkReply *reply = http.getStreaming(QNetworkRequest(QUrl(server.baseUrl().toString() + u"/event"_s)), 60'000);
    QByteArray received;
    QObject::connect(reply, &QNetworkReply::readyRead, [&] { received += reply->readAll(); });
    REQUIRE(QTest::qWaitFor([&] { return received.contains("{\"a\":2}"); }, 5'000));
    CHECK_FALSE(reply->isFinished());       // read while the server still holds the stream open
    CHECK(server.requestHead().startsWith("GET /v1/event "));
    CHECK(server.requestHead().contains("Accept: text/event-stream"));
    reply->abort();
    reply->deleteLater();
}

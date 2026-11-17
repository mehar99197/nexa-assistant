#include "remote/LocalApiServer.h"

#include "core/ai/ProviderRegistry.h"
#include "core/net/HttpClient.h"

#include "QtStringMakers.h"
#include "ScriptedProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>
#include <QUrl>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

Config apiConfig(bool enabled, const QString &token, quint16 port = 0) {
    Config c;
    c.providers = {ProviderConfig{
        .id = u"scripted"_s,
        .kind = u"scripted"_s,
        .name = u"Scripted"_s,
        .defaultModel = u"local-model"_s,
        .requiresKey = false,
    }};
    c.activeProvider = u"scripted"_s;
    c.localApi.enabled = enabled;
    c.localApi.port = port;
    c.localApi.bearerToken = token;
    return c;
}

struct HttpResult {
    int status{0};
    QByteArray body;
};

HttpResult syncRequest(const QByteArray &method, quint16 port, const QString &path, const QString &bearer,
                       const QByteArray &jsonBody = {}) {
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(u"http://127.0.0.1:%1%2"_s.arg(port).arg(path)));
    if (!bearer.isEmpty()) req.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    if (!jsonBody.isEmpty()) req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);

    QNetworkReply *reply = method == "GET" ? nam.get(req) : nam.post(req, jsonBody);
    QSignalSpy done(reply, &QNetworkReply::finished);
    REQUIRE(done.wait(3000));
    HttpResult res;
    res.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    res.body = reply->readAll();
    reply->deleteLater();
    return res;
}

} // namespace

TEST_CASE("LocalApiServer: loopback binding, bearer auth, OpenAI endpoints, and zero PC tools", "[remote][local-api]") {
    HttpClient http;
    ProviderRegistry registry(&http);
    QList<test::ScriptedProvider *> createdProviders;
    registry.registerKind(u"scripted"_s, [&](const ProviderConfig &, HttpClient *) {
        auto p = std::make_unique<test::ScriptedProvider>();
        p->repeat = test::ScriptedProvider::Reply{.text = u"Hello from local Nexa"_s};
        createdProviders.append(p.get());
        return std::unique_ptr<IAiProvider>(std::move(p));
    });

    const QString token = LocalApiServer::generateBearerToken();
    CHECK(token.size() >= 40);
    CHECK(LocalApiServer::matchesBearer("Bearer " + token.toUtf8(), token));
    CHECK_FALSE(LocalApiServer::matchesBearer("Bearer wrong", token));
    CHECK_FALSE(LocalApiServer::matchesBearer("", token));

    LocalApiServer server(&registry);
    // Disabled config fails empty-token guard or is not started unless enabled
    QString err;
    REQUIRE_FALSE(server.start(apiConfig(true, QString()), &err));

    REQUIRE(server.start(apiConfig(true, token, 0), &err));
    REQUIRE(server.isListening());
    const quint16 port = server.port();

    SECTION("rejects unauthorized requests with 401") {
        const HttpResult unauth = syncRequest("GET", port, u"/v1/models"_s, QString());
        CHECK(unauth.status == 401);
        const HttpResult badAuth = syncRequest("GET", port, u"/v1/models"_s, u"wrong-token"_s);
        CHECK(badAuth.status == 401);
    }

    SECTION("GET /v1/models lists provider models") {
        const HttpResult res = syncRequest("GET", port, u"/v1/models"_s, token);
        REQUIRE(res.status == 200);
        const QJsonObject doc = QJsonDocument::fromJson(res.body).object();
        CHECK(doc.value(u"object"_s).toString() == u"list"_s);
        REQUIRE_FALSE(doc.value(u"data"_s).toArray().isEmpty());
        CHECK(doc.value(u"data"_s).toArray().first().toObject().value(u"id"_s).toString() == u"m"_s);
    }

    SECTION("POST /v1/chat/completions answers JSON and strips all tools") {
        const QByteArray payload = QJsonDocument(QJsonObject{
            {u"model"_s, u"local-model"_s},
            {u"messages"_s, QJsonArray{QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, u"Hi there"_s}}}},
            {u"tools"_s, QJsonArray{QJsonObject{{u"type"_s, u"function"_s}}}},
        }).toJson(QJsonDocument::Compact);

        const HttpResult res = syncRequest("POST", port, u"/v1/chat/completions"_s, token, payload);
        REQUIRE(res.status == 200);
        const QJsonObject doc = QJsonDocument::fromJson(res.body).object();
        CHECK(doc.value(u"object"_s).toString() == u"chat.completion"_s);
        CHECK(doc.value(u"choices"_s).toArray().first().toObject()
                  .value(u"message"_s).toObject().value(u"content"_s).toString() == u"Hello from local Nexa"_s);
        REQUIRE_FALSE(createdProviders.isEmpty());
        CHECK(createdProviders.last()->requests.last().tools.isEmpty());
    }

    SECTION("POST /v1/chat/completions streams SSE chunks ending in [DONE]") {
        const QByteArray payload = QJsonDocument(QJsonObject{
            {u"model"_s, u"local-model"_s},
            {u"stream"_s, true},
            {u"messages"_s, QJsonArray{QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, u"Stream me"_s}}}},
        }).toJson(QJsonDocument::Compact);

        const HttpResult res = syncRequest("POST", port, u"/v1/chat/completions"_s, token, payload);
        REQUIRE(res.status == 200);
        CHECK(res.body.contains("chat.completion.chunk"));
        CHECK(res.body.contains("Hello from local Nexa"));
        CHECK(res.body.contains("data: [DONE]"));
    }

    SECTION("bounded LRU session pool never exceeds kMaxSessions") {
        for (int i = 0; i < LocalApiServer::kMaxSessions + 4; ++i) {
            const QByteArray payload = QJsonDocument(QJsonObject{
                {u"user"_s, u"session-%1"_s.arg(i)},
                {u"messages"_s, QJsonArray{QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, u"ping"_s}}}},
            }).toJson(QJsonDocument::Compact);
            const HttpResult res = syncRequest("POST", port, u"/v1/chat/completions"_s, token, payload);
            REQUIRE(res.status == 200);
        }
        CHECK(server.sessionCount() == LocalApiServer::kMaxSessions);
        CHECK_FALSE(server.hasSession(u"user:session-0|"_s));
        CHECK(server.hasSession(u"user:session-%1|"_s.arg(LocalApiServer::kMaxSessions + 3)));
    }
}

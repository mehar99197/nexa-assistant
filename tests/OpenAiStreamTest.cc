// End-to-end tests of OpenAiProvider over real HTTP against FakeHttpServer: stream decoding
// across reads, error mapping, plain-JSON fallback, cancellation and the model list.

#include "core/ai/OpenAiProvider.h"
#include "core/net/HttpClient.h"

#include "FakeHttpServer.h"
#include "QtStringMakers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <optional>

using namespace nexa;
using namespace nexa::test;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 5'000;

struct StreamOutcome {
    QString text;
    std::optional<FinishReason> finish;
    std::optional<Error> error;
    QList<ToolCall> toolCalls;
    int signalsAfterEnd{0};
};

ProviderConfig configFor(const QUrl &baseUrl) {
    return ProviderConfig{.id = u"fake"_s, .kind = u"openai-compatible"_s, .name = u"Fake"_s,
                          .baseUrl = baseUrl.toString(), .apiKey = u"sk-test"_s, .defaultModel = u"fake-model"_s};
}

StreamOutcome runChat(OpenAiProvider &provider, const std::function<void(ChatStream &)> &onFirstDelta = {}) {
    StreamOutcome outcome;
    std::shared_ptr<ChatStream> stream = provider.chat(ChatRequestBuilder().user(u"ping"_s).build());
    bool ended = false;
    QObject::connect(stream.get(), &ChatStream::textDelta, [&](const QString &delta) {
        if (ended) ++outcome.signalsAfterEnd;
        const bool first = outcome.text.isEmpty();
        outcome.text += delta;
        if (first && onFirstDelta) onFirstDelta(*stream);
    });
    QObject::connect(stream.get(), &ChatStream::finished, [&](FinishReason reason) {
        if (ended) ++outcome.signalsAfterEnd;
        ended = true;
        outcome.finish = reason;
        outcome.toolCalls = stream->toolCalls();
    });
    QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &error) {
        if (ended) ++outcome.signalsAfterEnd;
        ended = true;
        outcome.error = error;
    });
    REQUIRE(QTest::qWaitFor([&] { return ended; }, kWaitMs));
    QTest::qWait(30);  // let any late transport callbacks run
    return outcome;
}

QByteArray sse(const QByteArray &json) { return "data: " + json + "\n\n"; }

} // namespace

TEST_CASE("OpenAiChatStream streams text split across reads and finishes on [DONE]") {
    FakeHttpServer server;
    const QByteArray events = sse(R"({"choices":[{"index":0,"delta":{},"finish_reason":null}]})")  // OmniRoute keep-alive
        + sse(R"({"choices":[{"delta":{"role":"assistant"}}]})")
        + sse(R"({"choices":[{"delta":{"content":"p"}}]})")
        + sse(R"({"choices":[{"delta":{"content":"ong"}}]})")
        + sse(R"({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"total_tokens":9}})")
        + sse("[DONE]");
    // Cut the body at awkward places, including inside a "data:" line and between \n\n.
    server.respond({.chunks = {events.left(7), events.mid(7, 90), events.mid(97, 3), events.mid(100)}});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    const StreamOutcome outcome = runChat(provider);

    CHECK(outcome.text == u"pong"_s);
    REQUIRE(outcome.finish.has_value());
    CHECK(*outcome.finish == FinishReason::Stop);
    CHECK_FALSE(outcome.error.has_value());
    CHECK(outcome.signalsAfterEnd == 0);

    CHECK(server.requestHead().startsWith("POST /v1/chat/completions "));
    CHECK(server.requestHead().contains("Authorization: Bearer sk-test"));
    const QJsonObject body = QJsonDocument::fromJson(server.requestBody()).object();
    CHECK(body.value(u"model"_s).toString() == u"fake-model"_s);
    CHECK(body.value(u"stream"_s).toBool());
    CHECK(body.value(u"messages"_s).toArray().size() == 1);
}

TEST_CASE("OpenAiChatStream assembles streamed tool calls") {
    FakeHttpServer server;
    server.respond({.chunks = {
        sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","type":"function","function":{"name":"open_app","arguments":""}}]}}]})"),
        sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"name\":"}}]}}]})"),
        sse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"firefox\"}"}}]}}]})"),
        sse(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"),
        sse("[DONE]"),
    }});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    const StreamOutcome outcome = runChat(provider);

    REQUIRE(outcome.finish.has_value());
    CHECK(*outcome.finish == FinishReason::ToolCalls);
    REQUIRE(outcome.toolCalls.size() == 1);
    CHECK(outcome.toolCalls[0].id == u"c1"_s);
    CHECK(outcome.toolCalls[0].name == u"open_app"_s);
    CHECK(outcome.toolCalls[0].argumentsJson == u"{\"name\":\"firefox\"}"_s);
}

TEST_CASE("OpenAiChatStream maps HTTP 401 to Unauthorized with the server's message") {
    FakeHttpServer server;
    server.respond({.status = 401, .contentType = "application/json",
                    .chunks = {R"({"error":{"message":"Invalid API key"}})"}});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    const StreamOutcome outcome = runChat(provider);

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code == ErrorCode::Unauthorized);
    CHECK(outcome.error->message.contains(u"Invalid API key"_s));
    CHECK(outcome.text.isEmpty());
}

TEST_CASE("OpenAiChatStream reports an error event sent inside the stream") {
    FakeHttpServer server;
    server.respond({.chunks = {sse(R"({"choices":[{"delta":{"content":"par"}}]})"),
                               sse(R"({"error":{"message":"upstream overloaded"}})")}});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    const StreamOutcome outcome = runChat(provider);

    CHECK(outcome.text == u"par"_s);
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->message == u"upstream overloaded"_s);
}

TEST_CASE("OpenAiChatStream accepts a plain JSON reply from servers that ignore stream=true") {
    FakeHttpServer server;
    server.respond({.contentType = "application/json",
                    .chunks = {R"({"choices":[{"message":{"role":"assistant","content":"whole reply"},)",
                               R"("finish_reason":"stop"}]})"}});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    const StreamOutcome outcome = runChat(provider);

    CHECK(outcome.text == u"whole reply"_s);
    REQUIRE(outcome.finish.has_value());
    CHECK(*outcome.finish == FinishReason::Stop);
}

TEST_CASE("OpenAiChatStream finishes when the server closes without [DONE]") {
    FakeHttpServer server;
    server.respond({.chunks = {sse(R"({"choices":[{"delta":{"content":"ok"},"finish_reason":"length"}]})")}});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    const StreamOutcome outcome = runChat(provider);

    CHECK(outcome.text == u"ok"_s);
    REQUIRE(outcome.finish.has_value());
    CHECK(*outcome.finish == FinishReason::Length);
}

TEST_CASE("OpenAiChatStream fails when the server closes without any reply") {
    FakeHttpServer server;
    server.respond({.chunks = {": just a comment\n\n"}});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    const StreamOutcome outcome = runChat(provider);

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code == ErrorCode::Parse);
}

TEST_CASE("OpenAiChatStream cancel() ends with Cancelled and nothing after it") {
    FakeHttpServer server;
    server.respond({.chunks = {sse(R"({"choices":[{"delta":{"content":"first"}}]})"),
                               sse(R"({"choices":[{"delta":{"content":" second"}}]})")},
                    .holdOpen = true});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    const StreamOutcome outcome = runChat(provider, [](ChatStream &stream) { stream.cancel(); });

    CHECK(outcome.text == u"first"_s);
    REQUIRE(outcome.finish.has_value());
    CHECK(*outcome.finish == FinishReason::Cancelled);
    CHECK_FALSE(outcome.error.has_value());
    CHECK(outcome.signalsAfterEnd == 0);
}

TEST_CASE("OpenAiChatStream reports a refused connection") {
    quint16 closedPort = 0;
    {
        QTcpServer probe;
        REQUIRE(probe.listen(QHostAddress::LocalHost, 0));
        closedPort = probe.serverPort();
    }
    HttpClient http;
    OpenAiProvider provider(configFor(QUrl(u"http://127.0.0.1:%1/v1"_s.arg(closedPort))), &http);
    const StreamOutcome outcome = runChat(provider);

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code == ErrorCode::ConnectionRefused);
}

TEST_CASE("OpenAiProvider::listModels fetches, sorts and authenticates") {
    FakeHttpServer server;
    server.respond({.contentType = "application/json",
                    .chunks = {R"({"object":"list","data":[{"id":"zeta"},{"id":"alpha"}]})"}});

    HttpClient http;
    OpenAiProvider provider(configFor(server.baseUrl()), &http);
    std::optional<Result<QStringList>> models;
    provider.listModels([&](Result<QStringList> result) { models = std::move(result); });
    REQUIRE(QTest::qWaitFor([&] { return models.has_value(); }, kWaitMs));

    REQUIRE(models->isOk());
    CHECK(models->value() == QStringList{u"alpha"_s, u"zeta"_s});
    CHECK(server.requestHead().startsWith("GET /v1/models "));
    CHECK(server.requestHead().contains("Authorization: Bearer sk-test"));
}

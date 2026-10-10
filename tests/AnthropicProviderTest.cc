// core/ai/AnthropicProvider: the Messages API's request shape, its typed stream events, and a
// whole streamed exchange over real HTTP against FakeHttpServer.

#include "core/ai/AnthropicProvider.h"
#include "core/net/HttpClient.h"

#include "ChatTestUtil.h"
#include "FakeHttpServer.h"
#include "QtStringMakers.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::test;
using namespace Qt::StringLiterals;

namespace {

SseEvent event(const QByteArray &json) {
    return SseEvent{.type = u"message"_s, .data = json};
}

ProviderConfig configFor(const QUrl &baseUrl) {
    return ProviderConfig{.id = u"claude"_s, .kind = u"anthropic"_s, .name = u"Anthropic"_s,
                          .baseUrl = baseUrl.toString(), .apiKey = u"sk-ant-test"_s, .defaultModel = u"claude-test"_s};
}

} // namespace

TEST_CASE("Anthropic: the system prompt is a field, and tool calls and results are content blocks") {
    const ChatRequest request =
        ChatRequestBuilder()
            .model(u"claude-test"_s)
            .system(u"Be brief."_s)
            .user(u"Open firefox and look"_s, {ImageAttachment{"PNG", u"image/png"_s}})
            .assistant(u"Opening it."_s, {ToolCall{u"toolu_1"_s, u"open_app"_s, u"{\"name\":\"firefox\"}"_s},
                                          ToolCall{u"toolu_2"_s, u"take_screenshot"_s, u"{}"_s}})
            .toolResult(u"toolu_1"_s, u"open_app"_s, u"Opened Firefox"_s)
            .toolResult(u"toolu_2"_s, u"take_screenshot"_s, QString())
            .tools({ToolSpec{u"open_app"_s, u"Open an app"_s, schema::object({{u"name"_s, schema::string(u"App"_s)}})}})
            .build();
    const QJsonObject body = AnthropicProvider::buildChatBody(request, true);

    CHECK(body.value(u"system"_s).toString() == u"Be brief."_s);
    CHECK(body.value(u"max_tokens"_s).toInt() == AnthropicProvider::kDefaultMaxTokens);
    CHECK(body.value(u"stream"_s).toBool());
    const QJsonArray messages = body.value(u"messages"_s).toArray();
    REQUIRE(messages.size() == 3);     // no system message among them

    // The image goes before the words about it.
    const QJsonArray user = messages[0].toObject().value(u"content"_s).toArray();
    CHECK(user[0].toObject().value(u"type"_s).toString() == u"image"_s);
    CHECK(user[0].toObject().value(u"source"_s).toObject().value(u"media_type"_s).toString() == u"image/png"_s);
    CHECK(user[1].toObject().value(u"text"_s).toString() == u"Open firefox and look"_s);

    // The call's arguments travel as an object.
    const QJsonArray assistant = messages[1].toObject().value(u"content"_s).toArray();
    REQUIRE(assistant.size() == 3);
    CHECK(assistant[1].toObject().value(u"type"_s).toString() == u"tool_use"_s);
    CHECK(assistant[1].toObject().value(u"input"_s).toObject().value(u"name"_s).toString() == u"firefox"_s);

    // Both results in one user turn, and an empty one still says something.
    const QJsonObject results = messages[2].toObject();
    CHECK(results.value(u"role"_s).toString() == u"user"_s);
    const QJsonArray resultBlocks = results.value(u"content"_s).toArray();
    REQUIRE(resultBlocks.size() == 2);
    CHECK(resultBlocks[0].toObject().value(u"tool_use_id"_s).toString() == u"toolu_1"_s);
    CHECK(resultBlocks[1].toObject().value(u"content"_s).toString() == u"(no output)"_s);

    const QJsonObject tool = body.value(u"tools"_s).toArray()[0].toObject();
    CHECK(tool.value(u"input_schema"_s).toObject().value(u"type"_s).toString() == u"object"_s);
}

TEST_CASE("Anthropic: a conversation trimmed mid-exchange still opens with the user") {
    QList<ChatMessage> history{
        {.role = Role::Assistant, .text = u"earlier reply"_s, .toolCalls = {ToolCall{u"t0"_s, u"x"_s, u"{}"_s}}},
        {.role = Role::Tool, .text = u"orphaned result"_s, .toolCallId = u"t0"_s, .toolName = u"x"_s},
        {.role = Role::User, .text = u"next question"_s},
    };
    const QJsonArray messages = AnthropicProvider::buildChatBody(ChatRequestBuilder().messages(history).build(), false)
                                    .value(u"messages"_s)
                                    .toArray();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].toObject().value(u"role"_s).toString() == u"user"_s);
}

TEST_CASE("Anthropic: every kind of stream event is read") {
    CHECK(AnthropicProvider::decodeEvent(event(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hi"}})")).text
          == u"Hi"_s);

    const StreamDelta start = AnthropicProvider::decodeEvent(event(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_9","name":"open_app","input":{}}})"));
    REQUIRE(start.toolFragments.size() == 1);
    CHECK(start.toolFragments[0].index == 1);
    CHECK(start.toolFragments[0].id == u"toolu_9"_s);
    CHECK(start.toolFragments[0].argumentsDelta.isEmpty());    // the arguments come as deltas

    const StreamDelta json = AnthropicProvider::decodeEvent(event(
        R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"na"}})"));
    REQUIRE(json.toolFragments.size() == 1);
    CHECK(json.toolFragments[0].argumentsDelta == u"{\"na"_s);

    CHECK(*AnthropicProvider::decodeEvent(event(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})")).finishReason
          == FinishReason::ToolCalls);
    CHECK(AnthropicProvider::decodeEvent(event(R"({"type":"message_stop"})")).done);
    CHECK(AnthropicProvider::decodeEvent(event(R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"))
              .error->message == u"Overloaded"_s);

    // Thinking and keep-alives show nothing.
    const StreamDelta thinking = AnthropicProvider::decodeEvent(
        event(R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"hmm"}})"));
    CHECK(thinking.text.isEmpty());
    CHECK(AnthropicProvider::decodeEvent(event(R"({"type":"ping"})")).text.isEmpty());

    CHECK(AnthropicProvider::finishReasonFrom(u"end_turn"_s) == FinishReason::Stop);
    CHECK(AnthropicProvider::finishReasonFrom(u"max_tokens"_s) == FinishReason::Length);
    CHECK(AnthropicProvider::finishReasonFrom(u"refusal"_s) == FinishReason::ContentFilter);
}

TEST_CASE("Anthropic: a streamed answer with a tool call, end to end") {
    FakeHttpServer server;
    const QByteArray events =
        sseEvent(R"({"type":"message_start","message":{"id":"msg_1","role":"assistant","content":[]}})", "message_start")
        + sseEvent(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})", "content_block_start")
        + sseEvent(R"({"type":"ping"})", "ping")
        + sseEvent(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Opening "}})", "content_block_delta")
        + sseEvent(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Firefox."}})", "content_block_delta")
        + sseEvent(R"({"type":"content_block_stop","index":0})", "content_block_stop")
        + sseEvent(R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_01","name":"open_app","input":{}}})", "content_block_start")
        + sseEvent(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":""}})", "content_block_delta")
        + sseEvent(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"name\": \"fire"}})", "content_block_delta")
        + sseEvent(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"fox\"}"}})", "content_block_delta")
        + sseEvent(R"({"type":"content_block_stop","index":1})", "content_block_stop")
        + sseEvent(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":20}})", "message_delta")
        + sseEvent(R"({"type":"message_stop"})", "message_stop");
    server.respond({.chunks = {events.left(40), events.mid(40, 333), events.mid(373)}});

    HttpClient http;
    AnthropicProvider provider(configFor(server.baseUrl()), &http);
    const ChatOutcome outcome = runChat(provider, ChatRequestBuilder().system(u"sys"_s).user(u"open firefox"_s).build());

    CHECK_FALSE(outcome.error.has_value());
    CHECK(outcome.text == u"Opening Firefox."_s);
    REQUIRE(outcome.finish.has_value());
    CHECK(*outcome.finish == FinishReason::ToolCalls);
    REQUIRE(outcome.toolCalls.size() == 1);
    CHECK(outcome.toolCalls[0].id == u"toolu_01"_s);
    CHECK(outcome.toolCalls[0].name == u"open_app"_s);
    CHECK(QJsonDocument::fromJson(outcome.toolCalls[0].argumentsJson.toUtf8()).object().value(u"name"_s).toString()
          == u"firefox"_s);

    CHECK(server.requestHead().startsWith("POST /v1/messages "));
    // Header names are case-insensitive, and Qt writes them Title-Cased.
    const QByteArray head = server.requestHead().toLower();
    CHECK(head.contains("x-api-key: sk-ant-test"));
    CHECK(head.contains("anthropic-version: 2023-06-01"));
    // Not anthropic.com, so a gateway: the key goes as a bearer token too.
    CHECK(head.contains("authorization: bearer sk-ant-test"));
    const QJsonObject body = QJsonDocument::fromJson(server.requestBody()).object();
    CHECK(body.value(u"model"_s).toString() == u"claude-test"_s);
    CHECK(body.value(u"system"_s).toString() == u"sys"_s);
}

TEST_CASE("Anthropic: an error body says what went wrong") {
    FakeHttpServer server;
    server.respond({.status = 401, .contentType = "application/json",
                    .chunks = {R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})"}});
    HttpClient http;
    AnthropicProvider provider(configFor(server.baseUrl()), &http);
    const ChatOutcome outcome = runChat(provider, ChatRequestBuilder().user(u"hi"_s).build());
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code == ErrorCode::Unauthorized);
    CHECK(outcome.error->message.contains(u"invalid x-api-key"_s));
}

TEST_CASE("Anthropic: a gateway that answers with one whole message still works") {
    FakeHttpServer server;
    server.respond({.contentType = "application/json",
                    .chunks = {R"({"type":"message","content":[{"type":"text","text":"Done."},)"
                               R"({"type":"tool_use","id":"t1","name":"open_app","input":{"name":"x"}}],"stop_reason":"tool_use"})"}});
    HttpClient http;
    AnthropicProvider provider(configFor(server.baseUrl()), &http);
    const ChatOutcome outcome = runChat(provider, ChatRequestBuilder().user(u"hi"_s).build());
    CHECK(outcome.text == u"Done."_s);
    REQUIRE(outcome.toolCalls.size() == 1);
    CHECK(outcome.toolCalls[0].argumentsJson == u"{\"name\":\"x\"}"_s);
}

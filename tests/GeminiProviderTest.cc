// core/ai/GeminiProvider: generateContent's request shape, schemas Gemini accepts, its streamed
// responses, and a whole exchange over real HTTP against FakeHttpServer.

#include "core/ai/GeminiProvider.h"
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

QJsonObject object(const char *json) {
    return QJsonDocument::fromJson(json).object();
}

ProviderConfig configFor(const QUrl &baseUrl) {
    return ProviderConfig{.id = u"gemini"_s, .kind = u"gemini"_s, .name = u"Gemini"_s,
                          .baseUrl = baseUrl.toString(), .apiKey = u"AIza-test"_s, .defaultModel = u"gemini-test"_s};
}

} // namespace

TEST_CASE("Gemini: system instruction, user and model turns, calls and their results") {
    ToolCall call{u"call_0"_s, u"open_app"_s, u"{\"name\":\"firefox\"}"_s};
    call.signature = u"c2lnbmF0dXJl"_s;
    const ChatRequest request =
        ChatRequestBuilder()
            .system(u"Be brief."_s)
            .user(u"open firefox"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}})
            .assistant(QString(), {call, ToolCall{u"call_1"_s, u"take_screenshot"_s, u"{}"_s}})
            .toolResult(u"call_0"_s, u"open_app"_s, u"Opened Firefox"_s)
            .toolResult(u"call_1"_s, u"take_screenshot"_s, u"a screenshot"_s)
            .temperature(0.2)
            .build();
    const QJsonObject body = GeminiProvider::buildChatBody(request);

    CHECK(body.value(u"systemInstruction"_s).toObject().value(u"parts"_s).toArray()[0].toObject().value(u"text"_s).toString()
          == u"Be brief."_s);
    const QJsonArray contents = body.value(u"contents"_s).toArray();
    REQUIRE(contents.size() == 3);

    const QJsonArray userParts = contents[0].toObject().value(u"parts"_s).toArray();
    CHECK(contents[0].toObject().value(u"role"_s).toString() == u"user"_s);
    CHECK(userParts[1].toObject().value(u"inlineData"_s).toObject().value(u"mimeType"_s).toString() == u"image/jpeg"_s);

    // The model's turn: calls with their arguments as objects, and the signature handed back.
    const QJsonObject model = contents[1].toObject();
    CHECK(model.value(u"role"_s).toString() == u"model"_s);
    const QJsonArray calls = model.value(u"parts"_s).toArray();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].toObject().value(u"functionCall"_s).toObject().value(u"args"_s).toObject().value(u"name"_s).toString()
          == u"firefox"_s);
    CHECK(calls[0].toObject().value(u"thoughtSignature"_s).toString() == u"c2lnbmF0dXJl"_s);
    CHECK_FALSE(calls[1].toObject().contains(u"thoughtSignature"_s));

    // Both results in one user turn, matched by name, each an object.
    const QJsonArray results = contents[2].toObject().value(u"parts"_s).toArray();
    REQUIRE(results.size() == 2);
    const QJsonObject first = results[0].toObject().value(u"functionResponse"_s).toObject();
    CHECK(first.value(u"name"_s).toString() == u"open_app"_s);
    CHECK(first.value(u"response"_s).toObject().value(u"content"_s).toString() == u"Opened Firefox"_s);

    CHECK(body.value(u"generationConfig"_s).toObject().value(u"temperature"_s).toDouble() == 0.2);
}

TEST_CASE("Gemini: tool schemas are cut down to what it accepts") {
    const QJsonObject cleaned = GeminiProvider::schema(object(R"({
        "type": "object", "$schema": "http://json-schema.org/draft-07/schema#", "additionalProperties": false,
        "properties": {
            "level": {"type": "integer", "minimum": 0, "maximum": 100, "description": "Percent", "examples": [5]},
            "mode": {"type": "string", "enum": ["a", "b"], "default": "a"},
            "tags": {"type": "array", "items": {"type": "string", "pattern": "^x"}}
        },
        "required": ["level"]
    })"));
    CHECK_FALSE(cleaned.contains(u"$schema"_s));
    CHECK_FALSE(cleaned.contains(u"additionalProperties"_s));
    const QJsonObject properties = cleaned.value(u"properties"_s).toObject();
    CHECK(properties.value(u"level"_s).toObject() == object(R"({"type":"integer","minimum":0,"maximum":100,"description":"Percent"})"));
    CHECK_FALSE(properties.value(u"mode"_s).toObject().contains(u"default"_s));
    CHECK(properties.value(u"tags"_s).toObject().value(u"items"_s).toObject() == object(R"({"type":"string"})"));
    CHECK(cleaned.value(u"required"_s).toArray().size() == 1);

    // A tool without arguments sends no schema at all: Gemini refuses an empty object.
    CHECK(GeminiProvider::schema(object(R"({"type":"object","properties":{}})")).isEmpty());
    const ChatRequest request = ChatRequestBuilder().user(u"hi"_s).tools({ToolSpec{u"take_screenshot"_s, u"Look"_s, schema::object()}}).build();
    const QJsonObject declaration =
        GeminiProvider::buildChatBody(request).value(u"tools"_s).toArray()[0].toObject().value(u"functionDeclarations"_s).toArray()[0].toObject();
    CHECK(declaration.value(u"name"_s).toString() == u"take_screenshot"_s);
    CHECK_FALSE(declaration.contains(u"parameters"_s));
}

TEST_CASE("Gemini: streamed responses — text, whole calls with their signatures, thoughts left out") {
    const StreamDelta text = GeminiProvider::decodeResponse(
        object(R"({"candidates":[{"content":{"role":"model","parts":[{"text":"Hel"},{"text":"lo"}]}}]})"));
    CHECK(text.text == u"Hello"_s);
    CHECK_FALSE(text.finishReason.has_value());

    const StreamDelta calls = GeminiProvider::decodeResponse(object(R"({"candidates":[{"content":{"parts":[
        {"text":"thinking about it","thought":true},
        {"functionCall":{"name":"open_app","args":{"name":"files"}},"thoughtSignature":"sig-1"},
        {"functionCall":{"name":"take_screenshot","args":{}}}
    ]},"finishReason":"STOP"}]})"), 3);
    CHECK(calls.text.isEmpty());
    REQUIRE(calls.toolFragments.size() == 2);
    CHECK(calls.toolFragments[0].index == 3);            // numbered on from where the stream was
    CHECK(calls.toolFragments[0].signature == u"sig-1"_s);
    CHECK(calls.toolFragments[0].argumentsDelta == u"{\"name\":\"files\"}"_s);
    CHECK(calls.toolFragments[1].index == 4);
    CHECK(calls.toolFragments[1].argumentsDelta == u"{}"_s);
    CHECK(*calls.finishReason == FinishReason::Stop);

    CHECK(*GeminiProvider::decodeResponse(object(R"({"candidates":[{"finishReason":"MAX_TOKENS"}]})")).finishReason
          == FinishReason::Length);
    CHECK(*GeminiProvider::decodeResponse(object(R"({"candidates":[{"finishReason":"SAFETY"}]})")).finishReason
          == FinishReason::ContentFilter);
    CHECK(GeminiProvider::decodeResponse(object(R"({"promptFeedback":{"blockReason":"SAFETY"}})")).error->message.contains(u"SAFETY"_s));
    CHECK(GeminiProvider::decodeResponse(object(R"({"error":{"code":400,"message":"API key not valid"}})")).error->message
          == u"API key not valid"_s);
}

TEST_CASE("Gemini: only models that can chat are listed, without the models/ prefix") {
    const auto models = GeminiProvider::parseModels(R"({"models":[
        {"name":"models/gemini-2.5-flash","supportedGenerationMethods":["generateContent","countTokens"]},
        {"name":"models/text-embedding-004","supportedGenerationMethods":["embedContent"]},
        {"name":"models/gemini-2.5-pro","supportedGenerationMethods":["generateContent"]}
    ]})");
    REQUIRE(models);
    CHECK(models.value() == QStringList{u"gemini-2.5-flash"_s, u"gemini-2.5-pro"_s});
    CHECK_FALSE(GeminiProvider::parseModels("not json"));
}

TEST_CASE("Gemini: a streamed answer with a call, end to end, the key kept out of the URL") {
    FakeHttpServer server;
    server.respond({.chunks = {
        sseEvent(R"({"candidates":[{"content":{"role":"model","parts":[{"text":"Opening "}]}}]})"),
        sseEvent(R"({"candidates":[{"content":{"role":"model","parts":[{"text":"Files."}]}}]})"),
        sseEvent(R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"open_app","args":{"name":"files"}},"thoughtSignature":"abc"}]},"finishReason":"STOP"}]})"),
    }});
    HttpClient http;
    GeminiProvider provider(configFor(server.baseUrl()), &http);
    const ChatOutcome outcome = runChat(provider, ChatRequestBuilder().model(u"models/gemini-test"_s).user(u"open files"_s).build());

    CHECK_FALSE(outcome.error.has_value());
    CHECK(outcome.text == u"Opening Files."_s);
    REQUIRE(outcome.finish.has_value());
    CHECK(*outcome.finish == FinishReason::ToolCalls);       // Gemini says STOP; a call was made
    REQUIRE(outcome.toolCalls.size() == 1);
    CHECK(outcome.toolCalls[0].name == u"open_app"_s);
    CHECK(outcome.toolCalls[0].id == u"call_0"_s);            // Gemini gives no ids
    CHECK(outcome.toolCalls[0].signature == u"abc"_s);

    CHECK(server.requestHead().startsWith("POST /v1/models/gemini-test:streamGenerateContent?alt=sse "));
    CHECK(server.requestHead().toLower().contains("x-goog-api-key: aiza-test"));
    CHECK_FALSE(server.requestHead().contains("key=AIza"));
}

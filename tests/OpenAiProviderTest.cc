#include "core/ai/OpenAiProvider.h"

#include "QtStringMakers.h"

#include <QJsonArray>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("decodeChunk extracts content deltas") {
    const auto delta = OpenAiProvider::decodeChunk(
        R"({"choices":[{"index":0,"delta":{"content":"Hi"},"finish_reason":null}]})");
    CHECK(delta.text == u"Hi"_s);
    CHECK_FALSE(delta.finishReason.has_value());
    CHECK_FALSE(delta.done);
    CHECK_FALSE(delta.error.has_value());
}

TEST_CASE("decodeChunk recognises the DONE sentinel") {
    CHECK(OpenAiProvider::decodeChunk("[DONE]").done);
    CHECK(OpenAiProvider::decodeChunk(" [DONE]\n").done);
}

TEST_CASE("decodeChunk extracts tool-call fragments") {
    const auto delta = OpenAiProvider::decodeChunk(R"({"choices":[{"delta":{"tool_calls":[
        {"index":1,"id":"call_x","type":"function","function":{"name":"open_app","arguments":"{\"na"}}]}}]})");
    REQUIRE(delta.toolFragments.size() == 1);
    CHECK(delta.toolFragments[0].index == 1);
    CHECK(delta.toolFragments[0].id == u"call_x"_s);
    CHECK(delta.toolFragments[0].name == u"open_app"_s);
    CHECK(delta.toolFragments[0].argumentsDelta == u"{\"na"_s);
}

TEST_CASE("decodeChunk uses the array position when a server omits the index") {
    const auto delta = OpenAiProvider::decodeChunk(R"({"choices":[{"delta":{"tool_calls":[
        {"id":"a","function":{"name":"mute","arguments":"{}"}},
        {"id":"b","function":{"name":"open_app","arguments":"{}"}}]}}]})");
    REQUIRE(delta.toolFragments.size() == 2);
    CHECK(delta.toolFragments[0].index == 0);
    CHECK(delta.toolFragments[1].index == 1);
}

TEST_CASE("decodeChunk serialises object arguments") {
    const auto delta = OpenAiProvider::decodeChunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"name":"set_volume","arguments":{"level":30}}}]}}]})");
    REQUIRE(delta.toolFragments.size() == 1);
    CHECK(delta.toolFragments[0].argumentsDelta == u"{\"level\":30}"_s);
}

TEST_CASE("decodeChunk maps finish reasons") {
    const auto finish = [](const char *reason) {
        return OpenAiProvider::decodeChunk(
                   QByteArray(R"({"choices":[{"delta":{},"finish_reason":")") + reason + R"("}]})")
            .finishReason;
    };
    CHECK(finish("stop") == FinishReason::Stop);
    CHECK(finish("length") == FinishReason::Length);
    CHECK(finish("tool_calls") == FinishReason::ToolCalls);
    CHECK(finish("content_filter") == FinishReason::ContentFilter);
    CHECK(finish("something_new") == FinishReason::Other);
}

TEST_CASE("decodeChunk reads a whole non-streamed completion") {
    const auto delta = OpenAiProvider::decodeChunk(R"({"choices":[{"message":{"role":"assistant","content":"Done",
        "tool_calls":[{"id":"c1","type":"function","function":{"name":"mute","arguments":"{}"}}]},
        "finish_reason":"tool_calls"}]})");
    CHECK(delta.text == u"Done"_s);
    REQUIRE(delta.toolFragments.size() == 1);
    CHECK(delta.toolFragments[0].name == u"mute"_s);
    CHECK(delta.finishReason == FinishReason::ToolCalls);
}

TEST_CASE("decodeChunk joins text parts of array content") {
    const auto delta = OpenAiProvider::decodeChunk(
        R"({"choices":[{"delta":{"content":[{"type":"text","text":"a"},{"type":"thinking"},{"type":"text","text":"b"}]}}]})");
    CHECK(delta.text == u"ab"_s);
}

TEST_CASE("decodeChunk reports in-stream errors") {
    const auto delta = OpenAiProvider::decodeChunk(R"({"error":{"message":"model not found","type":"invalid_request_error"}})");
    REQUIRE(delta.error.has_value());
    CHECK(delta.error->message == u"model not found"_s);
}

TEST_CASE("decodeChunk skips malformed, empty and usage-only payloads") {
    for (const char *payload : {"{not json", "", R"({"choices":[],"usage":{"total_tokens":3}})"}) {
        const auto delta = OpenAiProvider::decodeChunk(payload);
        CHECK(delta.text.isEmpty());
        CHECK(delta.toolFragments.isEmpty());
        CHECK_FALSE(delta.error.has_value());
        CHECK_FALSE(delta.done);
    }
}

TEST_CASE("buildChatBody maps every message role") {
    const ChatRequest request =
        ChatRequestBuilder()
            .model(u"gpt-4o"_s)
            .system(u"Be brief."_s)
            .user(u"What is this?"_s, {ImageAttachment{.data = "JPEGDATA", .mimeType = u"image/jpeg"_s}})
            .assistant({}, {ToolCall{.id = u"c1"_s, .name = u"take_screenshot"_s, .argumentsJson = u"{}"_s}})
            .toolResult(u"c1"_s, u"take_screenshot"_s, u"image 1280x720"_s)
            .build();
    const QJsonObject body = OpenAiProvider::buildChatBody(request, true);
    CHECK(body.value(u"model"_s).toString() == u"gpt-4o"_s);
    CHECK(body.value(u"stream"_s).toBool());
    CHECK_FALSE(body.contains(u"temperature"_s));
    CHECK_FALSE(body.contains(u"max_tokens"_s));

    const QJsonArray messages = body.value(u"messages"_s).toArray();
    REQUIRE(messages.size() == 4);
    CHECK(messages[0].toObject().value(u"role"_s).toString() == u"system"_s);

    const QJsonArray parts = messages[1].toObject().value(u"content"_s).toArray();
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].toObject().value(u"text"_s).toString() == u"What is this?"_s);
    const QString expectedUrl = u"data:image/jpeg;base64,"_s + QString::fromLatin1(QByteArray("JPEGDATA").toBase64());
    CHECK(parts[1].toObject().value(u"image_url"_s).toObject().value(u"url"_s).toString() == expectedUrl);

    const QJsonObject assistant = messages[2].toObject();
    CHECK(assistant.value(u"content"_s).isNull());
    const QJsonObject call = assistant.value(u"tool_calls"_s).toArray().first().toObject();
    CHECK(call.value(u"id"_s).toString() == u"c1"_s);
    CHECK(call.value(u"function"_s).toObject().value(u"name"_s).toString() == u"take_screenshot"_s);

    const QJsonObject tool = messages[3].toObject();
    CHECK(tool.value(u"role"_s).toString() == u"tool"_s);
    CHECK(tool.value(u"tool_call_id"_s).toString() == u"c1"_s);
    CHECK(tool.value(u"content"_s).toString() == u"image 1280x720"_s);
}

TEST_CASE("buildChatBody sends plain-string content and optional parameters") {
    const ChatRequest request =
        ChatRequestBuilder().model(u"m"_s).user(u"hi"_s).temperature(0.2).maxTokens(256).build();
    const QJsonObject body = OpenAiProvider::buildChatBody(request, false);
    CHECK_FALSE(body.value(u"stream"_s).toBool());
    CHECK(body.value(u"messages"_s).toArray().first().toObject().value(u"content"_s).toString() == u"hi"_s);
    CHECK(body.value(u"temperature"_s).toDouble() == 0.2);
    CHECK(body.value(u"max_tokens"_s).toInt() == 256);
}

TEST_CASE("parseModels sorts, de-duplicates and accepts bare arrays") {
    const auto models = OpenAiProvider::parseModels(
        R"({"object":"list","data":[{"id":"b-model"},{"id":"A-model"},{"id":"b-model"}]})");
    REQUIRE(models.isOk());
    CHECK(models.value() == QStringList{u"A-model"_s, u"b-model"_s});

    const auto bare = OpenAiProvider::parseModels(R"(["x","y"])");
    REQUIRE(bare.isOk());
    CHECK(bare.value().size() == 2);

    CHECK_FALSE(OpenAiProvider::parseModels("<html>").isOk());
}

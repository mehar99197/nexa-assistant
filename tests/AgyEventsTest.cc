// core/ai/AgyEvents: agy's stream-json, as measured against agy 1.4.8 on 2026-10-27.

#include "core/ai/AgyEvents.h"

#include "QtStringMakers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("decode reads agy's init, text, tool and result events") {
    const AgyEvent init = AgyEvents::decode(
        R"({"event":"init","conversation_id":"2d91ed35","init":{"model":"gemini-3.8-flash-low","cwd":"/tmp/x","tools":["run_command"],"permission_mode":"request-review"}})");
    CHECK(init.kind == AgyEvent::Kind::Init);

    const AgyEvent text = AgyEvents::decode(
        R"({"event":"step_update","step_update":{"conversation_id":"2d91ed35","step_index":1,"state":"ACTIVE","step_type":"agent_response","text_delta":"OK"}})");
    CHECK(text.kind == AgyEvent::Kind::Text);
    CHECK(text.text == u"OK"_s);

    const AgyEvent tool = AgyEvents::decode(
        R"({"event":"step_update","step_update":{"conversation_id":"31d540c4","step_index":2,"state":"ERROR","step_type":"tool","tool_name":"run_command","tool_info":{"name":"run_command","parameters":{"CommandLine":"echo x"},"error":{"type":"TOOL_ERROR","message":"permission check failed"}}}})");
    CHECK(tool.kind == AgyEvent::Kind::ToolStep);
    CHECK(tool.toolName == u"run_command"_s);
    CHECK_FALSE(tool.ok);

    const AgyEvent result = AgyEvents::decode(
        R"({"event":"result","result":{"conversation_id":"2d91ed35","status":"SUCCESS","response":"OK\n","duration_seconds":2.27,"num_turns":1,"usage":{"input_tokens":6785,"output_tokens":1}}})");
    CHECK(result.kind == AgyEvent::Kind::Result);
    CHECK(result.ok);
    CHECK(result.response == u"OK\n"_s);
    CHECK(result.deniedActions.isEmpty());
}

TEST_CASE("decode reports denied actions and errors") {
    const AgyEvent denied = AgyEvents::decode(
        R"({"event":"result","result":{"status":"SUCCESS","response":"","denied_actions":[{"action":"command","display_name":"RunCommand"}]}})");
    CHECK(denied.ok);
    CHECK(denied.response.isEmpty());
    CHECK(denied.deniedActions == QStringList{u"RunCommand"_s});

    const AgyEvent failed = AgyEvents::decode(
        R"({"event":"result","result":{"response":"","status":"ERROR","error":"stream input message is missing the \"event\" field","num_turns":0}})");
    CHECK(failed.kind == AgyEvent::Kind::Result);
    CHECK_FALSE(failed.ok);
    CHECK(failed.error == u"stream input message is missing the \"event\" field"_s);
}

TEST_CASE("decode ignores what it does not know") {
    CHECK(AgyEvents::decode("").kind == AgyEvent::Kind::Ignored);
    CHECK(AgyEvents::decode("warning: ignoring unsupported stream input message event \"x\"").kind
          == AgyEvent::Kind::Ignored);
    CHECK(AgyEvents::decode(R"({"event":"step_update","step_update":{"step_type":"user_input","state":"DONE"}})").kind
          == AgyEvent::Kind::Ignored);
    // The final step of a reply carries its last piece of text with state DONE.
    CHECK(AgyEvents::decode(
              R"({"event":"step_update","step_update":{"state":"DONE","step_type":"agent_response","text_delta":"\n"}})")
              .text
          == u"\n"_s);
}

TEST_CASE("userTurn writes one line agy accepts") {
    const QString said = u"Say \"hi\"\nthen stop"_s;
    const QByteArray line = AgyEvents::userTurn(said);
    REQUIRE(line.endsWith('\n'));
    CHECK(line.count('\n') == 1);
    const QJsonObject object = QJsonDocument::fromJson(line).object();
    CHECK(object.value(u"event"_s).toString() == u"user"_s);
    const QJsonArray content = object.value(u"message"_s).toObject().value(u"content"_s).toArray();
    REQUIRE(content.size() == 1);
    CHECK(content.at(0).toObject().value(u"type"_s).toString() == u"text"_s);
    CHECK(content.at(0).toObject().value(u"text"_s).toString() == said);
}

TEST_CASE("parseModels reads agy models' output") {
    const QString output = u"Fetching available models...\ngemini-3.8-flash-high\tGemini 3.8 Flash (High)\n"
                           "claude-sonnet-4-6\tClaude Sonnet 4.6 (Thinking)\n"_s;
    CHECK(AgyEvents::parseModels(output) == QStringList{u"gemini-3.8-flash-high"_s, u"claude-sonnet-4-6"_s});
    CHECK(AgyEvents::parseModels(QString()).isEmpty());
}

TEST_CASE("exitError and resultError say what to do") {
    const Error signIn = AgyEvents::exitError(1, u"Print mode: not authenticated, trying silent auth\n"_s);
    CHECK(signIn.code == ErrorCode::Unauthorized);
    CHECK(signIn.message.contains(u"run agy once"_s));

    const Error crashed = AgyEvents::exitError(2, u"first line\nsegmentation fault\n"_s);
    CHECK(crashed.code == ErrorCode::ConnectionRefused);
    CHECK(crashed.message == u"agy stopped (exit 2): segmentation fault"_s);

    const Error quota = AgyEvents::resultError(u"RESOURCE_EXHAUSTED: quota exceeded"_s);
    CHECK(quota.code == ErrorCode::Http);
    CHECK(quota.message.startsWith(u"HTTP 429: agy: "_s));

    const Error other = AgyEvents::resultError(u"something broke"_s);
    CHECK(other.code == ErrorCode::Unknown);
    CHECK(other.message == u"agy: something broke"_s);
}

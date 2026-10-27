// core/tools/TextToolParser: tool calls written as text, for models that cannot make them natively.

#include "core/tools/TextToolParser.h"

#include "QtStringMakers.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

const QStringList kKnown{u"open_app"_s, u"set_volume"_s, u"take_screenshot"_s};

QJsonObject arguments(const ToolCall &call) {
    return QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object();
}

} // namespace

TEST_CASE("Calls in <tool_call> blocks are taken out of the reply") {
    const auto parsed = TextToolParser::parse(
        u"Sure, opening it.\n<tool_call>{\"name\": \"open_app\", \"arguments\": {\"name\": \"firefox\"}}</tool_call>\n"
        "<tool_call>\n{\"name\": \"set_volume\", \"arguments\": {\"level\": 30}}\n</tool_call>"_s,
        kKnown);
    CHECK(parsed.text == u"Sure, opening it."_s);
    REQUIRE(parsed.calls.size() == 2);
    CHECK(parsed.calls[0].id == u"text_call_1"_s);
    CHECK(parsed.calls[0].name == u"open_app"_s);
    CHECK(arguments(parsed.calls[0]).value(u"name"_s).toString() == u"firefox"_s);
    CHECK(parsed.calls[1].name == u"set_volume"_s);
    CHECK(arguments(parsed.calls[1]).value(u"level"_s).toInt() == 30);
}

TEST_CASE("The shapes models actually write are all understood") {
    // Arguments as a string of JSON, or under another key, or the call wrapped as a function.
    const auto asString = TextToolParser::parse(
        u"<tool_call>{\"name\": \"open_app\", \"arguments\": \"{\\\"name\\\": \\\"files\\\"}\"}</tool_call>"_s, kKnown);
    REQUIRE(asString.calls.size() == 1);
    CHECK(arguments(asString.calls[0]).value(u"name"_s).toString() == u"files"_s);

    const auto parameters = TextToolParser::parse(
        u"<tool_call>{\"tool\": \"set_volume\", \"parameters\": {\"change\": -10}}</tool_call>"_s, kKnown);
    REQUIRE(parameters.calls.size() == 1);
    CHECK(arguments(parameters.calls[0]).value(u"change"_s).toInt() == -10);

    const auto wrapped = TextToolParser::parse(
        u"<tool_call>{\"function\": {\"name\": \"take_screenshot\", \"arguments\": {}}}</tool_call>"_s, kKnown);
    REQUIRE(wrapped.calls.size() == 1);
    CHECK(wrapped.calls[0].argumentsJson == u"{}"_s);

    // No arguments at all is an empty object, not nothing.
    const auto bare = TextToolParser::parse(u"<tool_call>{\"name\": \"take_screenshot\"}</tool_call>"_s, kKnown);
    REQUIRE(bare.calls.size() == 1);
    CHECK(bare.calls[0].argumentsJson == u"{}"_s);
}

TEST_CASE("A reply that is nothing but a call counts, fenced or bare") {
    const auto fenced = TextToolParser::parse(
        u"```json\n{\"name\": \"open_app\", \"arguments\": {\"name\": \"terminal\"}}\n```"_s, kKnown);
    REQUIRE(fenced.calls.size() == 1);
    CHECK(fenced.text.isEmpty());

    const auto bare = TextToolParser::parse(u"  {\"name\": \"open_app\", \"arguments\": {\"name\": \"x\"}}  "_s, kKnown);
    REQUIRE(bare.calls.size() == 1);

    const auto several = TextToolParser::parse(
        u"[{\"name\": \"open_app\", \"arguments\": {\"name\": \"a\"}}, {\"name\": \"take_screenshot\"}]"_s, kKnown);
    CHECK(several.calls.size() == 2);
}

TEST_CASE("Anything that is not a call Nexa can make is left for the user to read") {
    // An unknown tool, broken JSON, and JSON in the middle of an explanation.
    const QString unknown = u"<tool_call>{\"name\": \"format_disk\", \"arguments\": {}}</tool_call>"_s;
    CHECK(TextToolParser::parse(unknown, kKnown).calls.isEmpty());
    CHECK(TextToolParser::parse(unknown, kKnown).text == unknown);
    CHECK(TextToolParser::parse(u"<tool_call>{\"name\": \"open_app\", </tool_call>"_s, kKnown).calls.isEmpty());

    const QString explanation = u"A tool call looks like {\"name\": \"open_app\", \"arguments\": {}} in JSON."_s;
    const auto prose = TextToolParser::parse(explanation, kKnown);
    CHECK(prose.calls.isEmpty());
    CHECK(prose.text == explanation);

    // A list that is only partly calls is not a list of calls.
    CHECK(TextToolParser::parse(u"[{\"name\": \"open_app\"}, {\"name\": \"rm\"}]"_s, kKnown).calls.isEmpty());
    CHECK(TextToolParser::parse(u"plain answer"_s, kKnown).text == u"plain answer"_s);
}

TEST_CASE("The instructions list each tool and its arguments") {
    ToolSpec volume{u"set_volume"_s, u"Set the speaker volume."_s,
                    schema::object({{u"level"_s, schema::integer(u"Percent"_s, 0, 100)},
                                    {u"mode"_s, schema::choice(u"How"_s, {u"fast"_s, u"slow"_s})}},
                                   {u"level"_s})};
    const QString text = TextToolParser::instructions({volume});
    CHECK(text.contains(u"<tool_call>"_s));
    CHECK(text.contains(u"- set_volume: Set the speaker volume."_s));
    CHECK(text.contains(u"level: integer, required, 0…100 — Percent"_s));
    CHECK(text.contains(u"mode: string, one of fast|slow — How"_s));
}

TEST_CASE("History for a model without tools carries calls and results as text") {
    QList<ChatMessage> history{
        {.role = Role::System, .text = u"be brief"_s},
        {.role = Role::User, .text = u"open firefox and mute"_s},
        {.role = Role::Assistant, .text = u"On it."_s,
         .toolCalls = {ToolCall{u"c1"_s, u"open_app"_s, u"{\"name\":\"firefox\"}"_s},
                       ToolCall{u"c2"_s, u"mute"_s, u"{}"_s}}},
        {.role = Role::Tool, .text = u"Opened Firefox"_s, .toolCallId = u"c1"_s, .toolName = u"open_app"_s},
        {.role = Role::Tool, .text = u"Muted"_s, .toolCallId = u"c2"_s, .toolName = u"mute"_s},
    };
    const QList<ChatMessage> text = TextToolParser::asText(history);
    REQUIRE(text.size() == 4);      // the two results became one user message
    CHECK(text[0].text == u"be brief"_s);
    CHECK(text[2].role == Role::Assistant);
    CHECK(text[2].toolCalls.isEmpty());
    CHECK(text[2].text.startsWith(u"On it.\n<tool_call>{"_s));
    CHECK(text[2].text.contains(u"\"name\":\"open_app\""_s));
    CHECK(text[3].role == Role::User);
    CHECK(text[3].text == u"<tool_result name=\"open_app\">\nOpened Firefox\n</tool_result>\n\n"
                          "<tool_result name=\"mute\">\nMuted\n</tool_result>"_s);
}

TEST_CASE("StreamFilter passes prose through as it arrives") {
    TextToolParser::StreamFilter filter;
    CHECK(filter.feed(u"Hello "_s) == u"Hello"_s);      // the space waits for what follows it
    CHECK(filter.feed(u"world"_s) == u" world"_s);
    CHECK(filter.shown() == u"Hello world"_s);
}

TEST_CASE("StreamFilter holds back a call and the partial tag before it") {
    TextToolParser::StreamFilter filter;
    CHECK(filter.feed(u"Opening it.<to"_s) == u"Opening it."_s);
    CHECK(filter.feed(u"ol_call>{\"name\": \"open_app\"}"_s).isEmpty());
    CHECK(filter.feed(u"</tool_call> and more"_s).isEmpty());
    CHECK(filter.shown() == u"Opening it."_s);
}

TEST_CASE("StreamFilter lets a '<' through once it is clearly not a tag") {
    TextToolParser::StreamFilter filter;
    CHECK(filter.feed(u"x <"_s) == u"x"_s);
    CHECK(filter.feed(u"= y"_s) == u" <= y"_s);
}

TEST_CASE("StreamFilter holds a reply that starts like a bare call") {
    TextToolParser::StreamFilter filter;
    CHECK(filter.feed(u"  {\"name\": \"open_app\""_s).isEmpty());
    CHECK(filter.feed(u", \"arguments\": {}}"_s).isEmpty());
    TextToolParser::StreamFilter fenced;
    CHECK(fenced.feed(u"```json\n{}"_s).isEmpty());
}

#include "core/ai/ToolCallAccumulator.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("ToolCallAccumulator concatenates argument fragments per index") {
    ToolCallAccumulator accumulator;
    accumulator.add({.index = 0, .id = u"call_a"_s, .name = u"open_app"_s, .argumentsDelta = u"{\"na"_s});
    accumulator.add({.index = 0, .argumentsDelta = u"me\":\"firefox\"}"_s});

    const QList<ToolCall> calls = accumulator.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].id == u"call_a"_s);
    CHECK(calls[0].name == u"open_app"_s);
    CHECK(calls[0].argumentsJson == u"{\"name\":\"firefox\"}"_s);
}

TEST_CASE("ToolCallAccumulator keeps interleaved calls apart, ordered by index") {
    ToolCallAccumulator accumulator;
    accumulator.add({.index = 1, .id = u"b"_s, .name = u"set_volume"_s, .argumentsDelta = u"{\"level\":"_s});
    accumulator.add({.index = 0, .id = u"a"_s, .name = u"open_app"_s, .argumentsDelta = u"{}"_s});
    accumulator.add({.index = 1, .argumentsDelta = u"30}"_s});

    const QList<ToolCall> calls = accumulator.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].id == u"a"_s);
    CHECK(calls[1].id == u"b"_s);
    CHECK(calls[1].argumentsJson == u"{\"level\":30}"_s);
}

TEST_CASE("ToolCallAccumulator fills in missing ids and empty arguments") {
    ToolCallAccumulator accumulator;
    accumulator.add({.index = 3, .name = u"take_screenshot"_s});

    const QList<ToolCall> calls = accumulator.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].id == u"call_3"_s);
    CHECK(calls[0].argumentsJson == u"{}"_s);
}

TEST_CASE("ToolCallAccumulator splits calls that reuse an index") {
    ToolCallAccumulator accumulator;
    CHECK(accumulator.add({.index = 0, .id = u"a"_s, .name = u"open_app"_s, .argumentsDelta = u"{\"name\":"_s}) == 0);
    CHECK(accumulator.add({.index = 0, .argumentsDelta = u"\"files\"}"_s}) == 0);
    CHECK(accumulator.add({.index = 0, .id = u"b"_s, .name = u"mute"_s, .argumentsDelta = u"{"_s}) == 1);
    CHECK(accumulator.add({.index = 0, .argumentsDelta = u"}"_s}) == 1);

    const QList<ToolCall> calls = accumulator.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].argumentsJson == u"{\"name\":\"files\"}"_s);
    CHECK(calls[1].name == u"mute"_s);
    CHECK(calls[1].argumentsJson == u"{}"_s);
}

TEST_CASE("ToolCallAccumulator ignores repeated ids and names") {
    ToolCallAccumulator accumulator;
    accumulator.add({.index = 0, .id = u"a"_s, .name = u"open_app"_s, .argumentsDelta = u"{"_s});
    accumulator.add({.index = 0, .id = u"a"_s, .name = u"open_app"_s, .argumentsDelta = u"}"_s});

    const QList<ToolCall> calls = accumulator.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == u"open_app"_s);
    CHECK(calls[0].argumentsJson == u"{}"_s);
}

TEST_CASE("ToolCallAccumulator::clear forgets calls and index mappings") {
    ToolCallAccumulator accumulator;
    accumulator.add({.index = 0, .id = u"a"_s, .name = u"x"_s});
    accumulator.add({.index = 0, .id = u"b"_s, .name = u"y"_s});
    accumulator.clear();
    CHECK(accumulator.isEmpty());
    CHECK(accumulator.add({.index = 0, .id = u"c"_s, .name = u"z"_s}) == 0);
}

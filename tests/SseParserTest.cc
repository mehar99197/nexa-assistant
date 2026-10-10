#include "core/net/SseParser.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QList<SseEvent> parseWhole(QByteArrayView stream) {
    SseParser parser;
    QList<SseEvent> events = parser.feed(stream);
    events += parser.finish();
    return events;
}

QList<SseEvent> parseSplitAt(QByteArrayView stream, qsizetype at) {
    SseParser parser;
    QList<SseEvent> events = parser.feed(stream.first(at));
    events += parser.feed(stream.sliced(at));
    events += parser.finish();
    return events;
}

// An OpenAI-style stream with CRLF endings, a keep-alive comment, a usage chunk and [DONE].
const QByteArray kOpenAiStream =
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\r\n\r\n"
    ": keep-alive\r\n\r\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"lo \\u00e9\"}}]}\r\n\r\n"
    "event: message\r\ndata: {\"choices\":[],\"usage\":{}}\r\n\r\n"
    "data: [DONE]\r\n\r\n";

} // namespace

TEST_CASE("SseParser dispatches an event at a blank line") {
    SseParser parser;
    CHECK(parser.feed("data: hello\n").isEmpty());
    const QList<SseEvent> events = parser.feed("\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == u"message"_s);
    CHECK(events[0].data == "hello");
}

TEST_CASE("SseParser joins multi-line data with newlines") {
    const QList<SseEvent> events = parseWhole("data: a\ndata:b\ndata\n\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "a\nb\n");
}

TEST_CASE("SseParser accepts LF, CRLF and CR line endings") {
    const QList<SseEvent> lf = parseWhole("data: x\n\ndata: y\n\n");
    REQUIRE(lf.size() == 2);
    CHECK(parseWhole("data: x\r\n\r\ndata: y\r\n\r\n") == lf);
    CHECK(parseWhole("data: x\r\rdata: y\r\r") == lf);
}

TEST_CASE("SseParser gives identical results for every split point") {
    const QList<SseEvent> expected = parseWhole(kOpenAiStream);
    REQUIRE(expected.size() == 4);
    CHECK(expected[3].data == "[DONE]");
    for (qsizetype at = 0; at <= kOpenAiStream.size(); ++at) {
        INFO("split at byte " << at);
        CHECK(parseSplitAt(kOpenAiStream, at) == expected);
    }
}

TEST_CASE("SseParser handles byte-by-byte delivery") {
    SseParser parser;
    QList<SseEvent> events;
    for (const char byte : kOpenAiStream) events += parser.feed(QByteArrayView(&byte, 1));
    events += parser.finish();
    CHECK(events == parseWhole(kOpenAiStream));
}

TEST_CASE("SseParser waits after a trailing CR, then treats a lone CR as a line end") {
    SseParser parser;
    CHECK(parser.feed("data: x\r").isEmpty());
    QList<SseEvent> events = parser.feed("\rdata: y\r\r");
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "x");
    events = parser.finish();
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "y");
}

TEST_CASE("SseParser ignores comments, retry and unknown fields") {
    const QList<SseEvent> events = parseWhole(": comment\nretry: 1000\nfoo: bar\ndata: x\n\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "x");
}

TEST_CASE("SseParser strips exactly one leading space from values") {
    const QList<SseEvent> events = parseWhole("data:  two\n\ndata:none\n\n");
    REQUIRE(events.size() == 2);
    CHECK(events[0].data == " two");
    CHECK(events[1].data == "none");
}

TEST_CASE("SseParser reports event types and keeps the last id") {
    const QList<SseEvent> events = parseWhole("event: content_block_delta\nid: 7\ndata: {}\n\ndata: next\n\n");
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == u"content_block_delta"_s);
    CHECK(events[0].id == u"7"_s);
    CHECK(events[1].type == u"message"_s);
    CHECK(events[1].id == u"7"_s);
}

TEST_CASE("SseParser skips events without data") {
    CHECK(parseWhole("event: ping\n\n").isEmpty());
}

TEST_CASE("SseParser strips a UTF-8 BOM, even when it arrives split") {
    SseParser parser;
    QList<SseEvent> events = parser.feed("\xEF\xBB");
    events += parser.feed("\xBF" "data: x\n\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "x");
}

TEST_CASE("SseParser::finish flushes an unterminated final event") {
    SseParser parser;
    CHECK(parser.feed("data: tail").isEmpty());
    const QList<SseEvent> events = parser.finish();
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "tail");
}

// core/mission/TaskTranscript: what a task said and did, kept so its transcript can be shown again.

#include "core/mission/TaskTranscript.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Streamed words are kept as one piece between other events") {
    using Kind = TranscriptEvent::Kind;
    TaskTranscript transcript;
    transcript.append({Kind::User, u"hello"_s});
    transcript.append({Kind::ReplyStart});
    transcript.append({Kind::Delta, u"Hel"_s});
    transcript.append({Kind::Delta, u"lo there"_s});
    transcript.append({Kind::Action, u"echo"_s, u"echoed"_s, true, 12});
    transcript.append({Kind::Delta, u"Done."_s});
    REQUIRE(transcript.events().size() == 5);
    CHECK(transcript.events().at(2).text == u"Hello there"_s);
    CHECK(transcript.events().at(3).ms == 12);
    CHECK(transcript.events().at(4).text == u"Done."_s);
    CHECK(transcript.lastReply() == u"Done."_s);
}

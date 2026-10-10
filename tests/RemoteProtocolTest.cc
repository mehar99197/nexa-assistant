// remote/RemoteProtocol: the web remote's messages. Anything a client sends that is not exactly
// right is refused, because the other end is a network.

#include "remote/RemoteProtocol.h"

#include "QtStringMakers.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QJsonObject parsed(const QByteArray &frame) {
    return QJsonDocument::fromJson(frame).object();
}

} // namespace

TEST_CASE("Client messages are read when they are exactly right") {
    const auto hello = remote::decode(R"({"t":"hello","token":"abc"})");
    REQUIRE(hello);
    CHECK(hello->type == remote::ClientMessage::Type::Hello);
    CHECK(hello->token == u"abc"_s);

    const auto text = remote::decode(R"({"t":"text","text":"  open firefox  "})");
    REQUIRE(text);
    CHECK(text->type == remote::ClientMessage::Type::Text);
    CHECK(text->text == u"open firefox"_s);

    CHECK(remote::decode(R"({"t":"cancel"})")->type == remote::ClientMessage::Type::Cancel);
    CHECK(remote::decode(R"({"t":"ping"})")->type == remote::ClientMessage::Type::Ping);
}

TEST_CASE("Anything else is refused, not guessed at") {
    CHECK_FALSE(remote::decode(""));
    CHECK_FALSE(remote::decode("not json"));
    CHECK_FALSE(remote::decode("[1,2,3]"));
    CHECK_FALSE(remote::decode(R"({"t":"reboot"})"));
    CHECK_FALSE(remote::decode(R"({"text":"no type"})"));
    CHECK_FALSE(remote::decode(R"({"t":"hello"})"));                 // no token
    CHECK_FALSE(remote::decode(R"({"t":"hello","token":42})"));      // not a string
    CHECK_FALSE(remote::decode(R"({"t":"text","text":"   "})"));     // nothing to do
    QByteArray huge = R"({"t":"text","text":")";
    huge += QByteArray(remote::kMaxTextFrame, 'a') + R"("})";
    CHECK_FALSE(remote::decode(huge));
}

TEST_CASE("Server messages carry their type and their fields") {
    CHECK(parsed(remote::welcome(u"Phone"_s, true, false))
          == QJsonObject{{u"t"_s, u"welcome"_s}, {u"version"_s, remote::kVersion}, {u"device"_s, u"Phone"_s},
                         {u"voice"_s, true}, {u"busy"_s, false}});
    CHECK(parsed(remote::accepted(u"open firefox"_s, u"phone"_s))
          == QJsonObject{{u"t"_s, u"accepted"_s}, {u"text"_s, u"open firefox"_s}, {u"source"_s, u"phone"_s}});
    CHECK(parsed(remote::delta(u"Hel"_s)).value(u"text"_s).toString() == u"Hel"_s);
    CHECK(parsed(remote::step(u"open_app — Firefox · 12 ms"_s, true)).value(u"ok"_s).toBool());
    CHECK(parsed(remote::finished(u"stop"_s)).value(u"reason"_s).toString() == u"stop"_s);
    CHECK(parsed(remote::speech(u"/speech/abc.wav"_s)).value(u"url"_s).toString() == u"/speech/abc.wav"_s);
    CHECK(parsed(remote::error(u"busy"_s, u"Nexa is busy"_s))
          == QJsonObject{{u"t"_s, u"error"_s}, {u"code"_s, u"busy"_s}, {u"text"_s, u"Nexa is busy"_s}});
    CHECK(parsed(remote::pong()) == QJsonObject{{u"t"_s, u"pong"_s}});
    // Text with quotes and newlines survives the trip.
    CHECK(parsed(remote::delta(u"say \"hi\"\nthen"_s)).value(u"text"_s).toString() == u"say \"hi\"\nthen"_s);
}

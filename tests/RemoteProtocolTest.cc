// remote/RemoteProtocol: the web remote's messages. Anything a client sends that is not exactly
// right is refused, because the other end is a network.

#include "remote/RemoteProtocol.h"

#include "QtStringMakers.h"

#include <QJsonArray>
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

TEST_CASE("The core's state travels to the phone, and an older PC's welcome still reads") {
    using Type = remote::ServerMessage::Type;
    CHECK(parsed(remote::welcome(u"Phone"_s, true, false, u"listen"_s)).value(u"mode"_s).toString() == u"listen"_s);
    CHECK_FALSE(parsed(remote::welcome(u"Phone"_s, true, false)).contains(u"mode"_s));
    CHECK(parsed(remote::state(u"act"_s, {u"pc"_s, u"tools"_s}))
          == QJsonObject{{u"t"_s, u"state"_s}, {u"mode"_s, u"act"_s}, {u"nodes"_s, QJsonArray{u"pc"_s, u"tools"_s}}});

    const auto state = remote::decodeServer(remote::state(u"act"_s, {u"pc"_s, u"tools"_s}));
    REQUIRE(state);
    CHECK(state->type == Type::State);
    CHECK(state->mode == u"act"_s);
    CHECK(state->nodes == QStringList{u"pc"_s, u"tools"_s});
    CHECK(remote::decodeServer(remote::welcome(u"Phone"_s, true, false, u"idle"_s))->mode == u"idle"_s);
    CHECK(remote::decodeServer(remote::welcome(u"Phone"_s, true, false))->mode.isEmpty());
    CHECK(remote::decodeServer(remote::state(u"idle"_s))->nodes.isEmpty());
}

TEST_CASE("The phone app reads every server message back, and writes what the page writes") {
    using Type = remote::ServerMessage::Type;
    const auto welcome = remote::decodeServer(remote::welcome(u"Phone"_s, true, false));
    REQUIRE(welcome);
    CHECK(welcome->type == Type::Welcome);
    CHECK(welcome->device == u"Phone"_s);
    CHECK(welcome->voice);
    CHECK_FALSE(welcome->busy);
    CHECK(welcome->version == remote::kVersion);

    const auto accepted = remote::decodeServer(remote::accepted(u"open firefox"_s, u"pc"_s));
    REQUIRE(accepted);
    CHECK(accepted->type == Type::Accepted);
    CHECK(accepted->text == u"open firefox"_s);
    CHECK(accepted->source == u"pc"_s);
    CHECK(remote::decodeServer(remote::delta(u"Hel"_s))->text == u"Hel"_s);
    const auto step = remote::decodeServer(remote::step(u"open_app — failed"_s, false));
    REQUIRE(step);
    CHECK(step->type == Type::Step);
    CHECK_FALSE(step->ok);
    CHECK(remote::decodeServer(remote::finished(u"cancelled"_s))->reason == u"cancelled"_s);
    CHECK(remote::decodeServer(remote::failed(u"no model"_s))->text == u"no model"_s);
    CHECK(remote::decodeServer(remote::transcript(u"volume up"_s))->text == u"volume up"_s);
    CHECK(remote::decodeServer(remote::speech(u"/speech/abc.wav"_s))->url == u"/speech/abc.wav"_s);
    CHECK(remote::decodeServer(remote::busy(true))->busy);
    const auto error = remote::decodeServer(remote::error(u"unauthorized"_s, u"Pair again."_s));
    REQUIRE(error);
    CHECK(error->type == Type::Error);
    CHECK(error->code == u"unauthorized"_s);
    CHECK(error->text == u"Pair again."_s);
    CHECK(remote::decodeServer(remote::pong())->type == Type::Pong);
    // A step without an `ok` field is fine; an unknown type or no JSON is not.
    CHECK(remote::decodeServer(R"({"t":"step","text":"x"})")->ok);
    CHECK_FALSE(remote::decodeServer(R"({"t":"reboot"})"));
    CHECK_FALSE(remote::decodeServer("nope"));
    CHECK_FALSE(remote::decodeServer("[]"));

    // The client side writes exactly what the server reads.
    CHECK(remote::decode(remote::hello(u"abc"_s))->token == u"abc"_s);
    CHECK(remote::decode(remote::text(u"open firefox"_s))->text == u"open firefox"_s);
    CHECK(remote::decode(remote::cancel())->type == remote::ClientMessage::Type::Cancel);
    CHECK(remote::decode(remote::ping())->type == remote::ClientMessage::Type::Ping);
}

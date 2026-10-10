// Small pure helpers of the system services and tool specs.

#include "core/agent/Conversation.h"
#include "core/ai/OpenAiProvider.h"
#include "core/tools/ToolSpec.h"
#include "system/audio/WpctlAudioController.h"
#include "system/media/MprisController.h"
#include "system/screen/ScreenCapture.h"

#include "QtStringMakers.h"

#include <QImage>
#include <QJsonArray>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("WpctlAudioController parses wpctl output") {
    const auto plain = WpctlAudioController::parse(u"Volume: 0.45\n"_s);
    REQUIRE(plain.isOk());
    CHECK(plain.value().percent == 45);
    CHECK_FALSE(plain.value().muted);
    const auto muted = WpctlAudioController::parse(u"Volume: 1.00 [MUTED]\n"_s);
    REQUIRE(muted.isOk());
    CHECK(muted.value().percent == 100);
    CHECK(muted.value().muted);
    CHECK_FALSE(WpctlAudioController::parse(u"Error: no sink"_s).isOk());
}

TEST_CASE("MprisController maps action names") {
    CHECK(MprisController::actionFromString(u"Play"_s) == MprisController::Action::Play);
    CHECK(MprisController::actionFromString(u"resume"_s) == MprisController::Action::Play);
    CHECK(MprisController::actionFromString(u"toggle"_s) == MprisController::Action::Toggle);
    CHECK(MprisController::actionFromString(u"skip"_s) == MprisController::Action::Next);
    CHECK_FALSE(MprisController::actionFromString(u"rewind"_s).has_value());
    CHECK(MprisController::methodName(MprisController::Action::Toggle) == u"PlayPause"_s);
}

TEST_CASE("ScreenCapture::encode downscales to the long side and records the scale") {
    QImage large(2560, 1440, QImage::Format_ARGB32);
    large.fill(Qt::darkCyan);
    const ScreenCapture::Shot shot = ScreenCapture::encode(large);
    CHECK(shot.imageSize == QSize(1280, 720));
    CHECK(shot.realSize == QSize(2560, 1440));
    CHECK(shot.scale == 2.0);
    CHECK(shot.jpeg.startsWith("\xFF\xD8"));

    QImage small(800, 600, QImage::Format_RGB32);
    small.fill(Qt::white);
    const ScreenCapture::Shot unscaled = ScreenCapture::encode(small);
    CHECK(unscaled.imageSize == QSize(800, 600));
    CHECK(unscaled.scale == 1.0);
}

TEST_CASE("ToolSpec maps to the OpenAI tools format") {
    const ToolSpec spec{u"set_volume"_s, u"Set the volume"_s,
                        schema::object({{u"level"_s, schema::integer(u"Percent"_s, 0, 100)}}, {u"level"_s})};
    const QJsonObject tool = spec.toOpenAi();
    CHECK(tool.value(u"type"_s).toString() == u"function"_s);
    const QJsonObject function = tool.value(u"function"_s).toObject();
    CHECK(function.value(u"name"_s).toString() == u"set_volume"_s);
    const QJsonObject parameters = function.value(u"parameters"_s).toObject();
    CHECK(parameters.value(u"type"_s).toString() == u"object"_s);
    CHECK(parameters.value(u"required"_s).toArray().first().toString() == u"level"_s);
    CHECK(parameters.value(u"properties"_s).toObject().value(u"level"_s).toObject().value(u"maximum"_s).toInt() == 100);

    const QJsonObject empty = ToolSpec{u"x"_s, u"y"_s, {}}.toOpenAi();
    CHECK(empty.value(u"function"_s).toObject().value(u"parameters"_s).toObject().value(u"type"_s).toString() == u"object"_s);

    const ChatRequest request = ChatRequestBuilder().model(u"m"_s).user(u"hi"_s).tools({spec}).build();
    const QJsonObject body = OpenAiProvider::buildChatBody(request, true);
    CHECK(body.value(u"tools"_s).toArray().size() == 1);
    CHECK(body.value(u"tool_choice"_s).toString() == u"auto"_s);
    CHECK_FALSE(OpenAiProvider::buildChatBody(ChatRequestBuilder().model(u"m"_s).build(), true).contains(u"tools"_s));
}

TEST_CASE("Conversation sends only the newest images") {
    Conversation conversation;
    conversation.addUser(u"first"_s, {ImageAttachment{"A", u"image/jpeg"_s}});
    conversation.addAssistant(u"ok"_s);
    conversation.addUser(u"second"_s, {ImageAttachment{"B", u"image/jpeg"_s}});
    const QList<ChatMessage> messages = conversation.buildRequest(u"m"_s).messages;
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].images.isEmpty());
    CHECK(messages[0].text.contains(u"1 earlier image(s) omitted"_s));
    REQUIRE(messages[2].images.size() == 1);
    CHECK(messages[2].images[0].data == "B");
    CHECK(conversation.messages()[0].images.size() == 1);  // the history itself keeps them
}

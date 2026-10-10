// core/intent/PhoneIntent: phone phrases on the fast path — and the ordinary sentences that must
// stay with the model, because a wrong match here can dial someone.

#include "core/intent/IntentRouter.h"
#include "core/intent/PcIntents.h"
#include "core/intent/PhoneIntents.h"

#include "QtStringMakers.h"

#include <QHash>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

class FakeCatalog : public IAppCatalog {
public:
    double score(const QString &query) const override { return query == u"firefox" ? 1.0 : 0.0; }
};

// The same order as app/main.cc: control first, then the phone, then the PC.
struct Fixture {
    Fixture() : router([](const QString &) { return true; }) {
        router.add(std::make_unique<ControlIntent>());
        router.add(std::make_unique<PhoneIntent>());
        router.add(std::make_unique<MediaIntent>());
        router.add(std::make_unique<AudioIntent>());
        router.add(std::make_unique<ScreenIntent>());
        router.add(std::make_unique<AppIntent>(&catalog));
    }

    std::optional<IntentMatch> route(const QString &text) const { return router.route(text); }
    QString tool(const QString &text) const {
        const std::optional<IntentMatch> match = router.route(text);
        return match ? match->toolName : u"<none>"_s;
    }

    FakeCatalog catalog;
    IntentRouter router;
};

} // namespace

TEST_CASE("Opening an app on the phone, said either way round") {
    const Fixture f;
    for (const auto &[said, app] : {std::pair{u"on my phone open youtube"_s, u"youtube"_s},
                                    std::pair{u"open whatsapp on my phone"_s, u"whatsapp"_s},
                                    std::pair{u"open the camera app on my phone"_s, u"camera"_s},
                                    std::pair{u"Hey Nexa, please launch Spotify on my phone."_s, u"spotify"_s}}) {
        const auto match = f.route(said);
        REQUIRE(match);
        CHECK(match->toolName == u"android_open_app"_s);
        CHECK(match->arguments.value(u"name"_s).toString() == app);
    }
    // The PC keeps its own phrases.
    CHECK(f.tool(u"open firefox"_s) == u"open_app"_s);
}

TEST_CASE("Looking at the phone takes a screenshot and asks the model") {
    const Fixture f;
    for (const QString &said : {u"see my phone"_s, u"look at my phone"_s, u"take a screenshot of my phone"_s,
                                u"phone screenshot"_s, u"check my phone screen"_s}) {
        const auto match = f.route(said);
        REQUIRE(match);
        CHECK(match->toolName == u"android_screenshot"_s);
        CHECK(match->forwardToAgent);
    }
    // A question is passed on as asked.
    const auto question = f.route(u"What's on my phone?"_s);
    REQUIRE(question);
    CHECK(question->toolName == u"android_screenshot"_s);
    CHECK(question->agentPrompt == u"What's on my phone?"_s);
    // "See this" is still the PC screen.
    CHECK(f.tool(u"see this"_s) == u"take_screenshot"_s);
}

TEST_CASE("The phone's volume by level or by step") {
    const Fixture f;
    const auto level = [&f](const QString &said) {
        const auto match = f.route(said);
        return match && match->toolName == u"android_volume" ? match->arguments.value(u"level"_s).toInt(-1) : -1;
    };
    CHECK(level(u"phone volume 30"_s) == 30);
    CHECK(level(u"set my phone volume to 80%"_s) == 80);
    CHECK(level(u"set the volume to 20 on my phone"_s) == 20);
    CHECK(level(u"phone volume 250"_s) == 100);

    const auto up = f.route(u"phone volume up"_s);
    REQUIRE(up);
    CHECK(up->arguments.value(u"change"_s).toInt() == 10);
    const auto down = f.route(u"turn down the volume on my phone"_s);
    REQUIRE(down);
    CHECK(down->arguments.value(u"change"_s).toInt() == -10);

    // Without the phone it is the PC's volume.
    CHECK(f.tool(u"volume 30"_s) == u"set_volume"_s);
}

TEST_CASE("Music on the phone, and music on the PC") {
    const Fixture f;
    const auto action = [&f](const QString &said) {
        const auto match = f.route(said);
        return match && match->toolName == u"android_media" ? match->arguments.value(u"action"_s).toString() : QString();
    };
    CHECK(action(u"pause my phone"_s) == u"pause"_s);
    CHECK(action(u"play music on my phone"_s) == u"play"_s);
    CHECK(action(u"resume the song on my phone"_s) == u"play"_s);
    CHECK(action(u"next song on my phone"_s) == u"next"_s);
    CHECK(action(u"phone play"_s) == u"play"_s);
    CHECK(action(u"previous track on my phone"_s) == u"previous"_s);

    CHECK(f.tool(u"pause the music"_s) == u"media_control"_s);
    CHECK(f.tool(u"play music"_s) == u"media_control"_s);
}

TEST_CASE("Calling: a number always, a name only when the phone is named") {
    const Fixture f;
    const auto to = [&f](const QString &said) {
        const auto match = f.route(said);
        return match && match->toolName == u"android_call" ? match->arguments.value(u"to"_s).toString() : QString();
    };
    CHECK(to(u"call 0300 1234567"_s) == u"0300 1234567"_s);
    CHECK(to(u"dial +92 300 1234567"_s) == u"+92 300 1234567"_s);
    CHECK(to(u"call 911"_s) == u"911"_s);
    CHECK(to(u"call mom on my phone"_s) == u"mom"_s);
    CHECK(to(u"ring Ali Khan from my phone"_s) == u"ali khan"_s);

    // Chat, not phone calls.
    CHECK(f.tool(u"call mom"_s) == u"<none>"_s);
    CHECK(f.tool(u"call it a day"_s) == u"<none>"_s);
    CHECK(f.tool(u"call me Ali"_s) == u"<none>"_s);
    CHECK(f.tool(u"what do you call a baby goat"_s) == u"<none>"_s);
}

TEST_CASE("Texting needs the phone named and the message introduced, and keeps its words") {
    const Fixture f;
    const auto text = f.route(u"Text Mom on my phone saying I'm running late, sorry!"_s);
    REQUIRE(text);
    CHECK(text->toolName == u"android_sms_send"_s);
    CHECK(text->arguments.value(u"to"_s).toString() == u"Mom"_s);
    CHECK(text->arguments.value(u"message"_s).toString() == u"I'm running late, sorry!"_s);

    // The words a politeness filter would take for padding are part of the message.
    const auto now = f.route(u"text Ali on my phone saying come home now"_s);
    REQUIRE(now);
    CHECK(now->arguments.value(u"message"_s).toString() == u"come home now"_s);

    // "and" inside the message does not make it a compound request.
    const auto shopping = f.route(u"send a text to Ali on my phone saying: bread and milk please"_s);
    REQUIRE(shopping);
    CHECK(shopping->toolName == u"android_sms_send"_s);
    CHECK(shopping->arguments.value(u"message"_s).toString() == u"bread and milk please"_s);

    CHECK(f.tool(u"text me later"_s) == u"<none>"_s);
    CHECK(f.tool(u"text mom saying hi"_s) == u"<none>"_s);      // no phone named: the model decides
    CHECK(f.tool(u"message received"_s) == u"<none>"_s);
}

TEST_CASE("Notifications and texts are read, then summarised by the model") {
    const Fixture f;
    for (const QString &said : {u"read my notifications"_s, u"phone notifications"_s, u"check my phone notifications"_s,
                                u"any notifications on my phone"_s}) {
        const auto match = f.route(said);
        REQUIRE(match);
        CHECK(match->toolName == u"android_notifications"_s);
        CHECK(match->forwardToAgent);
    }
    for (const QString &said : {u"read my texts"_s, u"check my sms"_s, u"show me my latest text messages"_s}) {
        const auto match = f.route(said);
        REQUIRE(match);
        CHECK(match->toolName == u"android_sms_read"_s);
        CHECK(match->forwardToAgent);
    }
}

TEST_CASE("Phone keys") {
    const Fixture f;
    const auto key = [&f](const QString &said) {
        const auto match = f.route(said);
        return match && match->toolName == u"android_key" ? match->arguments.value(u"key"_s).toString() : QString();
    };
    CHECK(key(u"lock my phone"_s) == u"lock"_s);
    CHECK(key(u"go home on my phone"_s) == u"home"_s);
    CHECK(key(u"phone back"_s) == u"back"_s);
}

TEST_CASE("Sentences about a phone are not phone commands") {
    const Fixture f;
    for (const QString &said : {u"what's my phone number"_s, u"my phone is slow"_s, u"is my phone charging"_s,
                                u"what is a phone"_s, u"see you later"_s, u"how do I open whatsapp on my phone"_s,
                                u"open whatsapp on my phone and call mom"_s}) {
        INFO(said.toStdString());
        CHECK(f.tool(said) == u"<none>"_s);
    }
}

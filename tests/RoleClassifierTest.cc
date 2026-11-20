// core/mission/RoleClassifier: which kind of agent leads a task, from its words alone.

#include "core/mission/RoleClassifier.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

void expect(AgentRole role, std::initializer_list<const char *> texts) {
    for (const char *text : texts) {
        INFO(text);
        CHECK(RoleClassifier::classify(QString::fromUtf8(text)) == role);
    }
}

} // namespace

TEST_CASE("Tasks about the web, code, the screen and the phone get their agent") {
    expect(AgentRole::Web, {"Register semester courses on mul.edu.pk", "Check inbox and draft replies to unread mail",
                            "search the web for cheap flights to Lahore", "open https://github.com/trending",
                            "log in to my bank website", "fill in the form on the registration page"});
    expect(AgentRole::Code, {"Refactor invoice module and run tests", "Write unit tests for auth guard",
                             "fix the bug in my python script", "why does cmake fail with this stack trace",
                             "implement a function that parses dates in C++"});
    expect(AgentRole::Screen, {"what's on my screen", "click the OK button in that dialog", "look at this window",
                               "take a screenshot and tell me what you see"});
    expect(AgentRole::Phone, {"on my phone open youtube", "read the latest sms on my phone",
                              "send a text message to Ali from my phone", "what's on my android"});
}

TEST_CASE("Everyday requests stay with the general agent") {
    expect(AgentRole::General, {"open calculator", "volume 50", "what is the weather like", "remind me tomorrow at 9",
                                "tell me a joke", "Read timetable and summarise", "what time is it",
                                "compress last week's recordings", "test my microphone"});
}

TEST_CASE("The phone wins over the web, the screen over the web, the web over code") {
    CHECK(RoleClassifier::classify(u"on my phone open the website google.com"_s) == AgentRole::Phone);
    CHECK(RoleClassifier::classify(u"click the login button on the website"_s) == AgentRole::Screen);
    CHECK(RoleClassifier::classify(u"find the python code on github.com"_s) == AgentRole::Web);
}

TEST_CASE("A tool says whose work it is") {
    CHECK(RoleClassifier::forTool(u"android_tap"_s) == AgentRole::Phone);
    CHECK(RoleClassifier::forTool(u"browser_click"_s) == AgentRole::Web);
    CHECK(RoleClassifier::forTool(u"take_screenshot"_s) == AgentRole::Screen);
    CHECK(RoleClassifier::forTool(u"mouse_click"_s) == AgentRole::Screen);
    CHECK(RoleClassifier::forTool(u"screen_snapshot"_s) == AgentRole::Screen);
    CHECK(RoleClassifier::forTool(u"screen_click"_s) == AgentRole::Screen);
    CHECK(RoleClassifier::forTool(u"open_app"_s) == AgentRole::General);
}

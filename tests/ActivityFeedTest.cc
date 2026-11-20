// core/mission/ActivityFeed: the live activity, newest last, capped, filtered by lane.

#include "core/mission/ActivityFeed.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

ActivityEvent event(Lane lane, const QString &text) {
    return ActivityEvent{QDateTime::currentDateTime(), ActivityKind::ToolStarted, lane, std::nullopt, {}, text, true};
}

} // namespace

TEST_CASE("The feed keeps the newest events up to its capacity, in order") {
    ActivityFeed feed(3);
    for (int i = 1; i <= 5; ++i) feed.push(event(Lane::Web, QString::number(i)));
    REQUIRE(feed.size() == 3);
    const QList<ActivityEvent> events = feed.events();
    CHECK(events.at(0).text == u"3"_s);
    CHECK(events.at(2).text == u"5"_s);
}

TEST_CASE("A lane shows its own events only") {
    ActivityFeed feed;
    feed.push(event(Lane::Web, u"navigate"_s));
    feed.push(event(Lane::Code, u"edit"_s));
    feed.push(event(Lane::Web, u"click"_s));
    const QList<ActivityEvent> web = feed.events(Lane::Web);
    REQUIRE(web.size() == 2);
    CHECK(web.at(1).text == u"click"_s);
    CHECK(feed.events(Lane::Voice).isEmpty());
    CHECK(feed.events().size() == 3);
}

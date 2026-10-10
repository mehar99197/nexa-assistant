// system/input/RemoteDesktopSession::locate: screenshot pixels → the position the desktop's
// RemoteDesktop portal is sent. This is the arithmetic that decides where a click lands, on a
// desktop whose fractional scale makes physical and logical pixels differ.

#include "system/input/RemoteDesktopSession.h"

#include "QtStringMakers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using Catch::Approx;

namespace {

using Stream = RemoteDesktopSession::Stream;
using Units = RemoteDesktopSession::StreamUnits;

// This machine: a 1920×1080 panel at 125 %, which GNOME lays out — and reports the stream as —
// 1536×864 logical pixels.
const QList<Stream> kLaptop{{47, QRect(0, 0, 1536, 864)}};
const QSize kLaptopPixels(1920, 1080);

} // namespace

TEST_CASE("GNOME is sent the stream's own pixels, which put the pointer on the intended spot") {
    // Measured live: sending logical coordinates put each click at 80 % of the intended position,
    // because mutter divides by the monitor's scale. In pixels, a screenshot pixel goes through as
    // itself (its centre, so the last one stays on the screen).
    const auto centre = RemoteDesktopSession::locate(QPoint(960, 540), kLaptopPixels, kLaptop, Units::Pixels);
    REQUIRE(centre);
    CHECK(centre->nodeId == 47u);
    CHECK(centre->position.x() == Approx(960.5));
    CHECK(centre->position.y() == Approx(540.5));

    const auto left = RemoteDesktopSession::locate(QPoint(400, 300), kLaptopPixels, kLaptop, Units::Pixels);
    REQUIRE(left);
    CHECK(left->position.x() == Approx(400.5));
    CHECK(left->position.y() == Approx(300.5));

    const auto last = RemoteDesktopSession::locate(QPoint(1919, 1079), kLaptopPixels, kLaptop, Units::Pixels);
    REQUIRE(last);
    CHECK(last->position.x() <= 1919.0);
    CHECK(last->position.y() <= 1079.0);
}

TEST_CASE("Other desktops are sent the logical coordinates the portal documents") {
    const auto centre = RemoteDesktopSession::locate(QPoint(960, 540), kLaptopPixels, kLaptop, Units::Logical);
    REQUIRE(centre);
    // The centre of physical pixel 960 is 960.5, which is 768.4 logical pixels at 1.25.
    CHECK(centre->position.x() == Approx(768.4));
    CHECK(centre->position.y() == Approx(432.4));

    const auto right = RemoteDesktopSession::locate(QPoint(1500, 800), kLaptopPixels, kLaptop, Units::Logical);
    REQUIRE(right);
    CHECK(right->position.x() == Approx(1200.4));
    CHECK(right->position.y() == Approx(640.4));
}

TEST_CASE("The corners of the screenshot stay on the screen") {
    for (const Units units : {Units::Logical, Units::Pixels}) {
        const auto first = RemoteDesktopSession::locate(QPoint(0, 0), kLaptopPixels, kLaptop, units);
        REQUIRE(first);
        CHECK(first->position.x() >= 0.0);
        CHECK(first->position.y() >= 0.0);
    }
    const auto last = RemoteDesktopSession::locate(QPoint(1919, 1079), kLaptopPixels, kLaptop, Units::Logical);
    REQUIRE(last);
    CHECK(last->position.x() < 1536.0);
    CHECK(last->position.y() < 864.0);

    // Out of range, as a model reading a resized image might send: held at the edge, not wrapped.
    const auto beyond = RemoteDesktopSession::locate(QPoint(5000, -40), kLaptopPixels, kLaptop, Units::Pixels);
    REQUIRE(beyond);
    CHECK(beyond->position.x() == Approx(1919.0));
    CHECK(beyond->position.y() == Approx(0.0));
}

TEST_CASE("At 100 % both units agree") {
    const QList<Stream> plain{{3, QRect(0, 0, 2560, 1440)}};
    for (const Units units : {Units::Logical, Units::Pixels}) {
        const auto point = RemoteDesktopSession::locate(QPoint(1234, 567), QSize(2560, 1440), plain, units);
        REQUIRE(point);
        CHECK(point->position.x() == Approx(1234.5));
        CHECK(point->position.y() == Approx(567.5));
    }
}

TEST_CASE("Two monitors: the point goes to the one that shows it, in that monitor's own coordinates") {
    // Side by side, both at 200 %: the screenshot is 7680 pixels wide, the layout 3840 logical.
    const QList<Stream> pair{{10, QRect(0, 0, 1920, 1080)}, {11, QRect(1920, 0, 1920, 1080)}};
    const QSize pixels(7680, 2160);

    const auto onLeft = RemoteDesktopSession::locate(QPoint(200, 400), pixels, pair, Units::Pixels);
    REQUIRE(onLeft);
    CHECK(onLeft->nodeId == 10u);
    CHECK(onLeft->position.x() == Approx(200.5));

    const auto onRight = RemoteDesktopSession::locate(QPoint(4040, 400), pixels, pair, Units::Pixels);
    REQUIRE(onRight);
    CHECK(onRight->nodeId == 11u);
    CHECK(onRight->position.x() == Approx(200.5));      // relative to the right-hand monitor
    CHECK(onRight->position.y() == Approx(400.5));

    const auto logical = RemoteDesktopSession::locate(QPoint(4040, 400), pixels, pair, Units::Logical);
    REQUIRE(logical);
    CHECK(logical->nodeId == 11u);
    CHECK(logical->position.x() == Approx(100.25));
}

TEST_CASE("A point in the gap between monitors goes to the nearest one") {
    // A tall monitor beside a short one leaves an empty corner under the short one.
    const QList<Stream> uneven{{20, QRect(0, 0, 1000, 500)}, {21, QRect(1000, 0, 1000, 1000)}};
    const auto gap = RemoteDesktopSession::locate(QPoint(400, 900), QSize(2000, 1000), uneven, Units::Logical);
    REQUIRE(gap);
    CHECK(gap->nodeId == 20u);
    CHECK(gap->position.y() == Approx(499.0));          // held at the bottom edge of the short one
}

TEST_CASE("Nothing to point into means no answer, not a guess") {
    CHECK_FALSE(RemoteDesktopSession::locate(QPoint(1, 1), kLaptopPixels, {}, Units::Pixels));
    CHECK_FALSE(RemoteDesktopSession::locate(QPoint(1, 1), QSize(), kLaptop, Units::Pixels));
    CHECK_FALSE(RemoteDesktopSession::locate(QPoint(1, 1), QSize(0, 1080), kLaptop, Units::Logical));
}

TEST_CASE("GNOME, under any of its names, is the desktop that reads pixels") {
    CHECK(RemoteDesktopSession::unitsFor(u"GNOME"_s) == Units::Pixels);
    CHECK(RemoteDesktopSession::unitsFor(u"ubuntu:GNOME"_s) == Units::Pixels);
    CHECK(RemoteDesktopSession::unitsFor(u"GNOME-Classic:GNOME"_s) == Units::Pixels);
    CHECK(RemoteDesktopSession::unitsFor(u"gnome"_s) == Units::Pixels);
    CHECK(RemoteDesktopSession::unitsFor(u"KDE"_s) == Units::Logical);
    CHECK(RemoteDesktopSession::unitsFor(u"sway"_s) == Units::Logical);
    CHECK(RemoteDesktopSession::unitsFor(QString()) == Units::Logical);
    // Not fooled by a desktop whose name merely contains the word.
    CHECK(RemoteDesktopSession::unitsFor(u"NOTGNOME"_s) == Units::Logical);
}

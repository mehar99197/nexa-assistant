// system/input/PortalInputBackend: the two conversions that are easy to get backwards.

#include "system/input/PortalInputBackend.h"

#include "proto/LinuxKeyCodes.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;

TEST_CASE("Tool button numbers become evdev button codes") {
    CHECK(PortalInputBackend::evdevButton(1) == BTN_LEFT);
    CHECK(PortalInputBackend::evdevButton(2) == BTN_RIGHT);
    CHECK(PortalInputBackend::evdevButton(3) == BTN_MIDDLE);
    // Anything else is a left click rather than an error halfway through a gesture.
    CHECK(PortalInputBackend::evdevButton(0) == BTN_LEFT);
    CHECK(PortalInputBackend::evdevButton(9) == BTN_LEFT);
}

TEST_CASE("Scrolling up in the tools is scrolling up on the desktop") {
    // The tools: positive dy scrolls up, positive dx scrolls right. The portal follows libinput,
    // where positive vertical steps scroll *down*.
    CHECK(PortalInputBackend::portalScrollSteps(0, 3) == std::pair{-3, 0});
    CHECK(PortalInputBackend::portalScrollSteps(0, -2) == std::pair{2, 0});
    CHECK(PortalInputBackend::portalScrollSteps(4, 0) == std::pair{0, 4});
    CHECK(PortalInputBackend::portalScrollSteps(-1, 1) == std::pair{-1, -1});
}

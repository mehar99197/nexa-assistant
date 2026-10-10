// ui/NexaCoreSpec, NexaTheme and NexaCoreWidget: the nine states, their names and timings as the
// motion spec gives them, how the controller's activity resolves to one state, where the moving
// parts are at a given moment, and that the theme fills every token in.

#include "ui/NexaCoreSpec.h"
#include "ui/NexaCoreWidget.h"
#include "ui/NexaTheme.h"

#include "QtStringMakers.h"

#include <QImage>
#include <QSet>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace nexa;
using namespace Qt::StringLiterals;
using Catch::Approx;

TEST_CASE("Every state has a name, a wire key and a colour token of its own") {
    QSet<QString> names;
    QSet<QString> keys;
    for (const CoreMode mode : kCoreModes) {
        names.insert(coreModeName(mode));
        keys.insert(coreModeKey(mode));
        CHECK(coreModeFromKey(coreModeKey(mode)) == mode);
    }
    CHECK(names.size() == kCoreModeCount);
    CHECK(keys.size() == kCoreModeCount);
    CHECK(coreModeName(CoreMode::FastPath) == u"Fast path"_s);
    CHECK(coreModeKey(CoreMode::Transcribing) == u"stt"_s);
    CHECK_FALSE(coreModeFromKey(u"dancing"_s));

    CHECK(coreNodesForTool(u"android_tap"_s) == PhoneNode);
    CHECK(coreNodesForTool(u"speak"_s) == VoiceNode);
    CHECK(coreNodesForTool(u"open_app"_s) == (PcNode | ToolsNode));
    CHECK(coreNodesForTool(u""_s) == NoNode);
}

TEST_CASE("The timings are the motion spec's") {
    CHECK(coreTiming(CoreMode::Idle).breathSeconds == Approx(5.2));
    CHECK(coreTiming(CoreMode::Listening).breathSeconds == Approx(1.1));
    CHECK(coreTiming(CoreMode::Transcribing).breathSeconds == Approx(2.4));
    CHECK(coreTiming(CoreMode::Thinking).breathSeconds == Approx(2.8));
    CHECK(coreTiming(CoreMode::Acting).breathSeconds == Approx(1.6));
    CHECK(coreTiming(CoreMode::Speaking).breathSeconds == Approx(0.8));
    CHECK(coreTiming(CoreMode::Offline).breathSeconds == Approx(3.4));
    CHECK(coreTiming(CoreMode::Idle).ringTurnSeconds == Approx(70.0));
    CHECK(coreTiming(CoreMode::WakeWord).ringTurnSeconds == Approx(50.0));
    CHECK(coreTiming(CoreMode::Thinking).ringTurnSeconds == Approx(30.0));
    CHECK(coreTiming(CoreMode::Idle).cometLapSeconds == Approx(16.0));
    CHECK(coreTiming(CoreMode::Acting).cometLapSeconds == Approx(2.2));
    // 30 Hz is enough for breathing; anything happening gets 60.
    for (const CoreMode mode : {CoreMode::Idle, CoreMode::WakeWord, CoreMode::Offline}) {
        CHECK(coreTiming(mode).frameRateHz == 30);
    }
    for (const CoreMode mode : {CoreMode::Listening, CoreMode::Thinking, CoreMode::Acting, CoreMode::Speaking}) {
        CHECK(coreTiming(mode).frameRateHz == 60);
    }
}

TEST_CASE("The activity resolves to one state, the one the user must see first") {
    CHECK(resolveCoreMode({}) == CoreMode::Idle);
    CHECK(resolveCoreMode({.wakeWord = true}) == CoreMode::WakeWord);
    CHECK(resolveCoreMode({.thinking = true, .wakeWord = true}) == CoreMode::Thinking);
    CHECK(resolveCoreMode({.fastPath = true, .thinking = true}) == CoreMode::FastPath);
    CHECK(resolveCoreMode({.acting = true, .fastPath = true, .thinking = true}) == CoreMode::Acting);
    CHECK(resolveCoreMode({.transcribing = true, .acting = true}) == CoreMode::Transcribing);
    CHECK(resolveCoreMode({.listening = true, .transcribing = true, .acting = true}) == CoreMode::Listening);
    CHECK(resolveCoreMode({.speaking = true, .listening = true}) == CoreMode::Speaking);
    CHECK(resolveCoreMode({.offline = true, .speaking = true, .listening = true, .acting = true}) == CoreMode::Offline);
}

TEST_CASE("The core breathes within its bounds and the rings turn at their pace") {
    for (const CoreMode mode : kCoreModes) {
        for (double t = 0.0; t < 12.0; t += 0.37) {
            const CoreFrame frame = coreFrame(mode, 0.5, t, t);
            if (mode == CoreMode::Offline) {
                CHECK(frame.coreScale >= 0.97 - 1e-9);
                CHECK(frame.coreScale <= 1.0 + 1e-9);
            } else {
                CHECK(frame.coreScale >= 1.02 - 1e-9);
                CHECK(frame.coreScale <= 1.07 + 1e-9);
            }
        }
    }
    // One breath per period: the same scale a period later.
    CHECK(coreFrame(CoreMode::Idle, 0, 1.3, 1.3).coreScale == Approx(coreFrame(CoreMode::Idle, 0, 1.3 + 5.2, 6.5).coreScale));
    CHECK(coreFrame(CoreMode::Idle, 0, 1.3, 1.3).coreScale != Approx(coreFrame(CoreMode::Idle, 0, 2.6, 2.6).coreScale));
    // The dotted ring: half a turn at 35 s, a full one at 70.
    CHECK(coreFrame(CoreMode::Idle, 0, 35.0, 35.0).ringAngle == Approx(180.0));
    CHECK(coreFrame(CoreMode::Idle, 0, 70.0, 70.0).ringAngle == Approx(0.0).margin(1e-9));
    CHECK(coreFrame(CoreMode::Thinking, 0, 15.0, 15.0).ringAngle == Approx(180.0));
    // The comet: 16 s a lap idle, 2.2 s acting, none while listening.
    CHECK(coreFrame(CoreMode::Idle, 0, 8.0, 8.0).cometAngle.value() == Approx(180.0));
    CHECK(coreFrame(CoreMode::Acting, 0, 1.1, 1.1).cometAngle.value() == Approx(180.0));
    CHECK_FALSE(coreFrame(CoreMode::Listening, 0, 1.0, 1.0).cometAngle);
}

TEST_CASE("Each state moves in its own shape") {
    // Listening: two rings of ticks swelling with the microphone, out of phase.
    const CoreFrame quiet = coreFrame(CoreMode::Listening, 0.0, 1.0, 1.0);
    const CoreFrame loud = coreFrame(CoreMode::Listening, 1.0, 1.0, 1.0);
    CHECK(quiet.tickSwell == Approx(0.25));
    CHECK(loud.tickSwell == Approx(1.0));
    CHECK(coreFrame(CoreMode::Listening, 7.0, 1.0, 1.0).tickSwell == Approx(1.0));     // level clamped
    CHECK(std::fmod(std::abs(loud.tickPhaseInner - loud.tickPhaseOuter), 2 * 3.14159265) > 0.1);
    CHECK_FALSE(coreFrame(CoreMode::Idle, 1.0, 1.0, 1.0).tickSwell > 0);
    // Speaking: the ticks swell on their own, nothing measures the voice.
    CHECK(coreFrame(CoreMode::Speaking, 0.0, 0.4, 0.4).tickSwell > 0.1);
    // Transcribing: a scan arc, one turn in 1.1 s.
    CHECK(coreFrame(CoreMode::Transcribing, 0, 0.55, 0.55).scanAngle.value() == Approx(180.0));
    CHECK_FALSE(coreFrame(CoreMode::Thinking, 0, 0.55, 0.55).scanAngle);
    // Thinking: three satellites, the middle one the other way round.
    const CoreFrame thinking = coreFrame(CoreMode::Thinking, 0, 1.0, 1.0);
    REQUIRE(thinking.satelliteAngles);
    CHECK((*thinking.satelliteAngles)[0] > 0);
    CHECK((*thinking.satelliteAngles)[1] < 0);
    CHECK((*thinking.satelliteAngles)[2] > 0);
    // Wake word: a sonar ping every 3.2 s, growing from 0.4 core radii and fading.
    const CoreFrame pingStart = coreFrame(CoreMode::WakeWord, 0, 3.2, 3.2);
    const CoreFrame pingLater = coreFrame(CoreMode::WakeWord, 0, 4.8, 4.8);
    CHECK(pingStart.sonarRadius == Approx(0.4));
    CHECK(pingLater.sonarRadius > 1.5);
    CHECK(pingLater.sonarOpacity < pingStart.sonarOpacity);
    CHECK(coreFrame(CoreMode::WakeWord, 0, 6.39, 6.39).sonarRadius == Approx(2.9).epsilon(0.01));
    // Acting: packets leave for the nodes every 1.1 s.
    CHECK(coreFrame(CoreMode::Acting, 0, 0.55, 0.55).packetProgress.value() == Approx(0.25));
    // Fast path: one packet in 0.3 s and one ping in 0.9 s, then rest.
    CHECK(coreFrame(CoreMode::FastPath, 0, 10.1, 0.1).packetProgress);
    CHECK(coreFrame(CoreMode::FastPath, 0, 10.1, 0.1).sonarRadius > 0);
    CHECK_FALSE(coreFrame(CoreMode::FastPath, 0, 10.5, 0.5).packetProgress);
    CHECK(coreFrame(CoreMode::FastPath, 0, 10.5, 0.5).sonarRadius > 0);
    CHECK(coreFrame(CoreMode::FastPath, 0, 11.0, 1.0).sonarRadius == 0);
    // Idle and offline: only the breath, the ring and (idle) the comet.
    const CoreFrame offline = coreFrame(CoreMode::Offline, 1.0, 2.0, 2.0);
    CHECK(offline.tickSwell == 0);
    CHECK_FALSE(offline.cometAngle);
    CHECK(offline.sonarRadius == 0);
}

TEST_CASE("The easings start at 0 and end at 1") {
    for (const auto ease : {easeInOut, easeOut, easeIn, easeCross}) {
        CHECK(ease(0.0) == Approx(0.0));
        CHECK(ease(1.0) == Approx(1.0));
        CHECK(ease(-3.0) == Approx(0.0));   // clamped
        CHECK(ease(7.0) == Approx(1.0));
    }
    CHECK(easeInOut(0.5) == Approx(0.5));
    CHECK(easeIn(0.5) == Approx(0.25));
    CHECK(easeOut(0.5) == Approx(0.875));
    CHECK(easeCross(0.5) == Approx(0.5));
}

TEST_CASE("The theme fills every token in, for both schemes") {
    for (const ThemeScheme scheme : {ThemeScheme::Dark, ThemeScheme::Light}) {
        const ThemeTokens tokens = NexaTheme::tokens(scheme);
        CHECK(tokens.ground.isValid());
        CHECK(tokens.text.isValid());
        CHECK(tokens.ground != tokens.panel);
        QSet<QString> colours;
        for (const CoreMode mode : kCoreModes) colours.insert(tokens.stateColor(mode).name());
        CHECK(colours.size() == kCoreModeCount);

        const QString sheet = NexaTheme::styleSheet(scheme);
        REQUIRE_FALSE(sheet.isEmpty());
        CHECK_FALSE(sheet.contains(u'@'));              // no token left unfilled
        CHECK(sheet.contains(u"nexa--NexaCoreWidget"_s));
        CHECK(sheet.contains(u"qproperty-idleColor: "_s + tokens.idle.name()));
        CHECK(sheet.contains(u"nexa--ChatView"_s));
        CHECK(sheet.contains(u"nexa--ActionLogDock"_s));
    }
    // The dark ground is the design's; the light one is light.
    CHECK(NexaTheme::tokens(ThemeScheme::Dark).ground.name() == u"#0d1014"_s);
    CHECK(NexaTheme::tokens(ThemeScheme::Light).ground.lightness() > 200);
    CHECK(NexaTheme::schemeFor(u"dark"_s) == ThemeScheme::Dark);
    CHECK(NexaTheme::schemeFor(u"light"_s) == ThemeScheme::Light);
    CHECK(NexaTheme::schemeKey(ThemeScheme::Light) == u"light"_s);
}

TEST_CASE("The widget paints a state in its colour, and a change cross-fades") {
    NexaCoreWidget core;
    core.setAnimated(false);
    const QImage idle = core.snapshot(CoreMode::Idle, 96).toImage();
    const QImage acting = core.snapshot(CoreMode::Acting, 96).toImage();
    REQUIRE_FALSE(idle.isNull());
    // Inside the disc, above the N and off the glint, the state's colour shows plain.
    const QColor idleCentre = idle.pixelColor(48, 36);
    const QColor actingCentre = acting.pixelColor(48, 36);
    CHECK(idleCentre == core.idleColor());
    CHECK(actingCentre == core.actColor());
    CHECK(idle.pixelColor(2, 2).alpha() == 0);      // nothing outside the halo

    CHECK(core.mode() == CoreMode::Idle);
    core.setMode(CoreMode::Listening);
    CHECK(core.mode() == CoreMode::Listening);
    // The fast-path snap stays for its ping; a state the user must see at once does not wait.
    core.setMode(CoreMode::FastPath);
    core.setMode(CoreMode::Idle);
    CHECK(core.mode() == CoreMode::FastPath);
    core.setMode(CoreMode::Listening);
    CHECK(core.mode() == CoreMode::Listening);
}

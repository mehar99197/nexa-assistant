#include "ui/NexaCoreSpec.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2 * kPi;

double turns(double seconds, double period) {
    return period > 0 ? std::fmod(360.0 * seconds / period, 360.0) : 0.0;
}

double phase(double seconds, double period) {
    return std::fmod(seconds, period) / period;
}

} // namespace

QString coreStyleName(CoreStyle style) {
    switch (style) {
    case CoreStyle::Classic: return u"Classic"_s;
    case CoreStyle::Orbit: return u"Orbit"_s;
    case CoreStyle::Pulse: return u"Pulse"_s;
    case CoreStyle::Aurora: return u"Aurora"_s;
    case CoreStyle::Comet: return u"Comet"_s;
    case CoreStyle::Nebula: return u"Nebula"_s;
    case CoreStyle::Storm: return u"Storm"_s;
    case CoreStyle::Minimal: return u"Minimal"_s;
    }
    return {};
}

QString coreStyleKey(CoreStyle style) {
    return coreStyleName(style).toLower();
}

QString coreStyleBlurb(CoreStyle style) {
    switch (style) {
    case CoreStyle::Classic: return u"The motion spec as written: a breath, a slow ring, one comet."_s;
    case CoreStyle::Orbit: return u"Three satellites circle the core at all times; the comet runs faster."_s;
    case CoreStyle::Pulse: return u"A sonar ping leaves the core every 2.4 s, whatever the state."_s;
    case CoreStyle::Aurora: return u"The colour drifts around the state's own, and the halo sways."_s;
    case CoreStyle::Comet: return u"Two comets chase each other round the ring."_s;
    case CoreStyle::Nebula: return u"A deeper breath and a halo that swells with it."_s;
    case CoreStyle::Storm: return u"Level ticks flicker in every state and the ring turns three times as fast."_s;
    case CoreStyle::Minimal: return u"No ring, no comet, a shallow breath: the core alone."_s;
    }
    return {};
}

std::optional<CoreStyle> coreStyleFromKey(QStringView key) {
    for (const CoreStyle style : kCoreStyles) {
        if (coreStyleKey(style) == key) return style;
    }
    return std::nullopt;
}

QString coreShapeName(CoreShape shape) {
    switch (shape) {
    case CoreShape::Ring: return u"Ring"_s;
    case CoreShape::Orb: return u"Orb"_s;
    case CoreShape::Hex: return u"Hex"_s;
    case CoreShape::Bars: return u"Bars"_s;
    case CoreShape::Iris: return u"Iris"_s;
    case CoreShape::Crystal: return u"Crystal"_s;
    case CoreShape::Arc: return u"Arc"_s;
    case CoreShape::Dot: return u"Dot"_s;
    }
    return {};
}

QString coreShapeKey(CoreShape shape) {
    return coreShapeName(shape).toLower();
}

std::optional<CoreShape> coreShapeFromKey(QStringView key) {
    for (const CoreShape shape : kCoreShapes) {
        if (coreShapeKey(shape) == key) return shape;
    }
    return std::nullopt;
}

int coreStyleFrameRate(CoreStyle style, CoreMode mode) {
    const int base = coreTiming(mode).frameRateHz;
    return style == CoreStyle::Storm ? 60 : base;
}

QString coreModeName(CoreMode mode) {
    switch (mode) {
    case CoreMode::Idle: return u"Idle"_s;
    case CoreMode::WakeWord: return u"Wake word"_s;
    case CoreMode::Listening: return u"Listening"_s;
    case CoreMode::Transcribing: return u"Transcribing"_s;
    case CoreMode::Thinking: return u"Thinking"_s;
    case CoreMode::Acting: return u"Acting"_s;
    case CoreMode::Speaking: return u"Speaking"_s;
    case CoreMode::FastPath: return u"Fast path"_s;
    case CoreMode::Offline: return u"Offline"_s;
    }
    return {};
}

QString coreModeKey(CoreMode mode) {
    switch (mode) {
    case CoreMode::Idle: return u"idle"_s;
    case CoreMode::WakeWord: return u"wake"_s;
    case CoreMode::Listening: return u"listen"_s;
    case CoreMode::Transcribing: return u"stt"_s;
    case CoreMode::Thinking: return u"think"_s;
    case CoreMode::Acting: return u"act"_s;
    case CoreMode::Speaking: return u"speak"_s;
    case CoreMode::FastPath: return u"fast"_s;
    case CoreMode::Offline: return u"offline"_s;
    }
    return {};
}

std::optional<CoreMode> coreModeFromKey(QStringView key) {
    for (const CoreMode mode : kCoreModes) {
        if (coreModeKey(mode) == key) return mode;
    }
    return std::nullopt;
}

CoreNodes coreNodesForTool(QStringView tool) {
    if (tool.isEmpty()) return NoNode;
    if (tool.startsWith(u"android_")) return PhoneNode;
    if (tool == u"speak") return VoiceNode;
    return PcNode | ToolsNode;
}

QString coreNodeName(CoreNode node) {
    switch (node) {
    case VoiceNode: return u"Voice"_s;
    case PcNode: return u"PC"_s;
    case ToolsNode: return u"Tools"_s;
    case PhoneNode: return u"Phone"_s;
    case NoNode: break;
    }
    return {};
}

CoreTiming coreTiming(CoreMode mode) {
    switch (mode) {
    case CoreMode::Idle: return {5.2, 70.0, 16.0, 30};
    case CoreMode::WakeWord: return {5.2, 50.0, 16.0, 30};
    case CoreMode::Listening: return {1.1, 70.0, 0.0, 60};
    case CoreMode::Transcribing: return {2.4, 70.0, 0.0, 60};
    case CoreMode::Thinking: return {2.8, 30.0, 8.0, 60};
    case CoreMode::Acting: return {1.6, 70.0, 2.2, 60};
    case CoreMode::Speaking: return {0.8, 70.0, 0.0, 60};
    case CoreMode::FastPath: return {1.6, 70.0, 0.0, 60};
    case CoreMode::Offline: return {3.4, 70.0, 0.0, 30};
    }
    return {5.2, 70.0, 16.0, 30};
}

CoreMode resolveCoreMode(const CoreActivity &a) {
    if (a.offline) return CoreMode::Offline;
    if (a.speaking) return CoreMode::Speaking;
    if (a.listening) return CoreMode::Listening;
    if (a.transcribing) return CoreMode::Transcribing;
    if (a.acting) return CoreMode::Acting;
    if (a.fastPath) return CoreMode::FastPath;
    if (a.thinking) return CoreMode::Thinking;
    if (a.wakeWord) return CoreMode::WakeWord;
    return CoreMode::Idle;
}

double easeInOut(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}

double easeOut(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return 1 - std::pow(1 - x, 3);
}

double easeIn(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x;
}

double easeCross(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3 - 2 * x);
}

namespace {

// What a style adds on top of the state's own motion.
void applyStyle(CoreFrame &frame, CoreStyle style, CoreMode mode, double seconds) {
    const CoreTiming timing = coreTiming(mode);
    switch (style) {
    case CoreStyle::Classic:
        break;
    case CoreStyle::Orbit:
        if (!frame.satelliteAngles) frame.satelliteAngles = {{turns(seconds, 6.4), -turns(seconds, 9.4), turns(seconds, 13.2)}};
        if (frame.cometAngle) frame.cometAngle = turns(seconds, timing.cometLapSeconds * 0.75);
        break;
    case CoreStyle::Pulse:
        if (frame.sonarRadius <= 0) {
            const double p = phase(seconds, 2.4);
            frame.sonarRadius = 0.4 + 2.5 * easeOut(p);
            frame.sonarOpacity = 0.35 * (1 - p);
        }
        break;
    case CoreStyle::Aurora:
        frame.hueShift = 25.0 * std::sin(kTau * seconds / 9.0);
        frame.haloScale = 1.08 + 0.06 * std::sin(kTau * seconds / 5.0);
        break;
    case CoreStyle::Comet: {
        const double lap = timing.cometLapSeconds > 0 ? timing.cometLapSeconds : 12.0;
        frame.cometAngle = turns(seconds, lap);
        frame.secondCometAngle = std::fmod(*frame.cometAngle + 180.0, 360.0);
        break;
    }
    case CoreStyle::Nebula:
        frame.coreScale = std::min(1.14, 1.0 + (frame.coreScale - 1.0) * 2.0);
        frame.haloScale = 1.15 + 0.1 * std::sin(kTau * seconds / timing.breathSeconds);
        break;
    case CoreStyle::Storm:
        frame.tickSwell = std::max(frame.tickSwell, 0.25 + 0.15 * std::sin(kTau * seconds * 1.7)
                                                        + 0.1 * std::sin(kTau * seconds * 3.1));
        if (frame.tickPhaseInner == 0.0) frame.tickPhaseInner = kTau * seconds / 0.7;
        if (frame.tickPhaseOuter == 0.0) frame.tickPhaseOuter = kTau * seconds / 1.1 + kPi;
        frame.ringAngle = turns(seconds, timing.ringTurnSeconds / 3.0);
        break;
    case CoreStyle::Minimal:
        frame.ring = false;
        frame.cometAngle.reset();
        if (mode != CoreMode::Thinking) frame.satelliteAngles.reset();
        frame.coreScale = 1.0 + (frame.coreScale - 1.0) * 0.5;
        break;
    }
}

} // namespace

CoreFrame coreFrame(CoreMode mode, double level, double seconds, double secondsInMode, CoreStyle style) {
    const CoreTiming timing = coreTiming(mode);
    level = std::clamp(level, 0.0, 1.0);
    CoreFrame frame;

    // The breath: a sine eased at the turns, scale 1.02 … 1.07; offline, 0.97 … 1.0.
    const double breath = easeInOut(0.5 + 0.5 * std::sin(kTau * seconds / timing.breathSeconds));
    frame.coreScale = mode == CoreMode::Offline ? 0.97 + 0.03 * breath : 1.02 + 0.05 * breath;

    frame.ringAngle = turns(seconds, timing.ringTurnSeconds);
    if (timing.cometLapSeconds > 0) frame.cometAngle = turns(seconds, timing.cometLapSeconds);

    switch (mode) {
    case CoreMode::Listening:
        // Two rings of 48 ticks swell with the microphone, each breathing at its own pace, out of phase.
        frame.tickSwell = 0.25 + 0.75 * level;
        frame.tickPhaseInner = kTau * seconds / 0.9;
        frame.tickPhaseOuter = kTau * seconds / 1.3 + kPi;
        break;
    case CoreMode::Speaking:
        // Nothing measures the voice, so the ticks follow a 1.6 s swell, the outer ring 0.8 s behind.
        frame.tickSwell = 0.45 + 0.35 * std::sin(kTau * seconds / 1.6);
        frame.tickPhaseInner = kTau * seconds / 1.6;
        frame.tickPhaseOuter = kTau * (seconds + 0.8) / 1.6;
        break;
    case CoreMode::Transcribing:
        frame.scanAngle = turns(seconds, 1.1);
        break;
    case CoreMode::Thinking:
        frame.satelliteAngles = {{turns(seconds, 3.2), -turns(seconds, 4.7), turns(seconds, 6.6)}};
        break;
    case CoreMode::WakeWord: {
        // A sonar ping every 3.2 s: grows from 0.4 to 2.9 core radii and fades.
        const double p = phase(seconds, 3.2);
        frame.sonarRadius = 0.4 + 2.5 * easeOut(p);
        frame.sonarOpacity = 0.55 * (1 - p);
        break;
    }
    case CoreMode::Acting:
        // Packets leave the core for the lit nodes, one every 1.1 s.
        frame.packetProgress = easeIn(phase(seconds, 1.1));
        break;
    case CoreMode::FastPath: {
        // Once per request: a packet in 0.3 s and a ping in 0.9 s, then rest.
        if (secondsInMode < 0.3) frame.packetProgress = easeIn(secondsInMode / 0.3);
        if (secondsInMode < 0.9) {
            const double p = secondsInMode / 0.9;
            frame.sonarRadius = 0.4 + 2.5 * easeOut(p);
            frame.sonarOpacity = 0.7 * (1 - p);
        }
        break;
    }
    case CoreMode::Idle:
    case CoreMode::Offline:
        break;
    }
    applyStyle(frame, style, mode, seconds);
    return frame;
}

} // namespace nexa
